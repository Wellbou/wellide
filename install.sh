#!/bin/sh
# Wellide installer for Linux.
#   curl -fsSL https://raw.githubusercontent.com/Wellbou/wellide/main/install.sh | sh
# Options: --no-tun (skip TUN permissions)  --uninstall
# Env:     WELLIDE_REF=main|v0.2.0   PREFIX=/usr/local   SINGBOX_VERSION=1.14.2
set -eu

REPO=Wellbou/wellide
REF=${WELLIDE_REF:-main}
PREFIX=${PREFIX:-/usr/local}
SB_VER=${SINGBOX_VERSION:-1.14.2}
TUN=1
UNINSTALL=0
for a in "$@"; do
    case $a in
        --no-tun) TUN=0 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) sed -n 2,5p "$0"; exit 0 ;;
    esac
done

c() { printf '\033[35m%s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" = 0 ]; then SUDO=""; else
    command -v sudo >/dev/null 2>&1 && SUDO=sudo || { command -v doas >/dev/null 2>&1 && SUDO=doas || die "need sudo or doas"; }
fi
USER_NAME=${SUDO_USER:-$(id -un)}

if [ $UNINSTALL = 1 ]; then
    c "removing wellide"
    for f in bin/wellide share/applications/wellide.desktop share/icons/hicolor/scalable/apps/wellide.svg \
             share/icons/hicolor/scalable/apps/wellide-on.svg share/metainfo/io.github.wellbou.wellide.metainfo.xml; do
        $SUDO rm -f "$PREFIX/$f"
    done
    for s in 16 32 48 64 128 256; do $SUDO rm -f "$PREFIX/share/icons/hicolor/${s}x${s}/apps/wellide.png"; done
    $SUDO rm -rf /usr/local/lib/wellide
    $SUDO rm -f /etc/polkit-1/rules.d/49-wellide.rules
    c "done (settings kept in ~/.config/wellide)"
    exit 0
fi

# ---------- dependencies ----------
. /etc/os-release 2>/dev/null || true
ID_ALL="${ID:-} ${ID_LIKE:-}"
c "installing build dependencies ($ID_ALL)"
case $ID_ALL in
    *arch*|*manjaro*|*endeavouros*)
        $SUDO pacman -Syu --needed --noconfirm base-devel pkgconf gtk3 json-glib libsoup3 libcap curl polkit
        $SUDO pacman -S --needed --noconfirm libayatana-appindicator 2>/dev/null || true
        command -v sing-box >/dev/null 2>&1 || $SUDO pacman -S --needed --noconfirm sing-box 2>/dev/null || true ;;
    *debian*|*ubuntu*|*mint*|*pop*)
        $SUDO apt-get update -qq
        $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential pkg-config libgtk-3-dev \
            libjson-glib-dev libcap2-bin curl ca-certificates tar
        $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libsoup-3.0-dev \
            || die "libsoup-3.0 is not available on this release (Debian 12+ / Ubuntu 22.04+ needed)"
        $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libayatana-appindicator3-dev 2>/dev/null || true ;;
    *fedora*|*rhel*|*centos*)
        $SUDO dnf install -y gcc make pkgconf-pkg-config gtk3-devel json-glib-devel libsoup3-devel libcap curl tar
        $SUDO dnf install -y libayatana-appindicator-gtk3-devel 2>/dev/null || true ;;
    *suse*)
        $SUDO zypper -n install gcc make pkgconf gtk3-devel json-glib-devel libsoup-devel libcap-progs curl tar
        $SUDO zypper -n install libayatana-appindicator3-devel 2>/dev/null || true ;;
    *alpine*)
        $SUDO apk add build-base pkgconf gtk+3.0-dev json-glib-dev libsoup3-dev libcap curl tar
        $SUDO apk add libayatana-appindicator-dev 2>/dev/null || true ;;
    *)
        c "unknown distro: make sure gtk3, json-glib, libsoup3 dev packages are installed" ;;
esac

# ---------- sing-box ----------
if ! command -v sing-box >/dev/null 2>&1; then
    case $(uname -m) in
        x86_64) ARCH=amd64 ;; aarch64|arm64) ARCH=arm64 ;; armv7*) ARCH=armv7 ;; i?86) ARCH=386 ;;
        *) die "no sing-box build for $(uname -m); install it manually" ;;
    esac
    c "installing sing-box $SB_VER"
    T=$(mktemp -d)
    curl -fsSL "https://github.com/SagerNet/sing-box/releases/download/v$SB_VER/sing-box-$SB_VER-linux-$ARCH.tar.gz" | tar -xz -C "$T"
    $SUDO install -Dm755 "$T"/sing-box-*/sing-box "$PREFIX/bin/sing-box"
    rm -rf "$T"
fi
SB=$(command -v sing-box || echo "$PREFIX/bin/sing-box")

# ---------- build ----------
if [ -f Makefile ] && [ -f src/wellide.h ]; then SRC=$(pwd); else
    SRC=$(mktemp -d)
    c "downloading wellide ($REF)"
    case $REF in v*) URL="https://github.com/$REPO/archive/refs/tags/$REF.tar.gz" ;;
                 *)  URL="https://github.com/$REPO/archive/refs/heads/$REF.tar.gz" ;; esac
    curl -fsSL "$URL" | tar -xz -C "$SRC" --strip-components=1
fi
c "building"
make -C "$SRC" -j"$(nproc 2>/dev/null || echo 2)"
$SUDO make -C "$SRC" install PREFIX="$PREFIX"
command -v gtk-update-icon-cache >/dev/null 2>&1 && $SUDO gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
command -v update-desktop-database >/dev/null 2>&1 && $SUDO update-desktop-database -q "$PREFIX/share/applications" 2>/dev/null || true

# ---------- TUN permissions (same as the in-app button) ----------
if [ $TUN = 1 ]; then
    c "granting TUN permissions (sing-box copy with CAP_NET_ADMIN)"
    GID=$(id -g "$USER_NAME")
    $SUDO install -D -o root -g "$GID" -m 0750 "$SB" /usr/local/lib/wellide/sing-box
    $SUDO setcap cap_net_admin,cap_net_raw,cap_net_bind_service+ep /usr/local/lib/wellide/sing-box
    if [ -d /etc/polkit-1/rules.d ]; then
        getent group wellide >/dev/null 2>&1 || $SUDO groupadd -r wellide
        $SUDO usermod -aG wellide "$USER_NAME" 2>/dev/null || $SUDO adduser "$USER_NAME" wellide
        $SUDO install -Dm644 "$SRC/data/49-wellide.rules" /etc/polkit-1/rules.d/49-wellide.rules
    fi
fi

c "Wellide installed. Launch it from the menu or run: wellide"
