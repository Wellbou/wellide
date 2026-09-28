#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build wellide_<ver>_<arch>.deb with the sing-box core bundled.
# Usage: packaging/debian/build-deb.sh <path-to-sing-box-binary>
set -e
SB=$1
[ -x "$SB" ] || { echo "usage: $0 <sing-box binary>" >&2; exit 1; }
V=$(grep -o 'WL_VERSION "[^"]*"' src/wellide.h | cut -d'"' -f2)
ARCH=$(dpkg --print-architecture)
R=build/deb
rm -rf $R && mkdir -p $R/DEBIAN
make -j"$(nproc)" PREFIX=/usr
make install PREFIX=/usr DESTDIR=$R
install -Dm755 "$SB" $R/usr/lib/wellide/sing-box
install -Dm644 LICENSE $R/usr/share/doc/wellide/copyright
strip --strip-unneeded $R/usr/bin/wellide

# runtime deps straight from the binary
mkdir -p build/debian && printf 'Source: wellide\n\nPackage: wellide\n' > build/debian/control
DEPS=$(cd build && dpkg-shlibdeps -O ../$R/usr/bin/wellide 2>/dev/null | sed 's/^shlibs:Depends=//')
rm -rf build/debian

cat > $R/DEBIAN/control <<CTL
Package: wellide
Version: $V
Architecture: $ARCH
Maintainer: Wellbou <wellbou@users.noreply.github.com>
Depends: $DEPS, glib-networking
Recommends: libcap2-bin, pkexec | policykit-1
Section: net
Priority: optional
Homepage: https://github.com/Wellbou/wellide
Installed-Size: $(du -sk $R/usr | cut -f1)
Description: lightweight VPN client for sing-box subscriptions
 Wellide is a small GTK3 front end for sing-box: import a subscription
 (VLESS, VMess, Trojan, Shadowsocks, Hysteria2, TUIC), pick a server or let
 it choose the fastest, and connect in system-proxy or TUN mode.
 The sing-box core is bundled.
CTL

cat > $R/DEBIAN/postinst <<'SH'
#!/bin/sh
set -e
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
# refresh the capable TUN copy after an upgrade (created by the in-app button)
if [ -x /usr/local/lib/wellide/sing-box ] && command -v setcap >/dev/null; then
    install -o root -g "$(stat -c %g /usr/local/lib/wellide/sing-box)" -m 0750 /usr/lib/wellide/sing-box /usr/local/lib/wellide/sing-box
    setcap cap_net_admin,cap_net_raw,cap_net_bind_service+ep /usr/local/lib/wellide/sing-box || true
fi
exit 0
SH
cat > $R/DEBIAN/postrm <<'SH'
#!/bin/sh
set -e
if [ "$1" = purge ]; then
    rm -rf /usr/local/lib/wellide
    rm -f /etc/polkit-1/rules.d/49-wellide.rules /etc/polkit-1/rules.d/49-wellide-user-*.rules
fi
exit 0
SH
chmod 755 $R/DEBIAN/postinst $R/DEBIAN/postrm
dpkg-deb --root-owner-group -Zxz --build $R "wellide_${V}_${ARCH}.deb"
dpkg-deb --info "wellide_${V}_${ARCH}.deb"
