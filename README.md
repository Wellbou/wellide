# Wellide

A small VPN client for Linux, written in C with GTK3 and using [sing-box](https://sing-box.sagernet.org) as the core. It does what I used Hiddify for and uses about a sixth of the memory.

| | Hiddify | Wellide |
|---|---|---|
| GUI process | ~370 MB | ~60 MB |
| Core | built-in | sing-box, ~50–80 MB |
| Binary | ~100 MB Flutter bundle | 110 KB |

## Features

- Subscriptions over `http(s)://`. It reads the `subscription-userinfo`, `profile-title` and `profile-update-interval` headers, so you see traffic used, expiry, the provider's profile name and automatic updates.
- Links: `vless://` (ws, grpc, httpupgrade, h2, tcp, reality), `vmess://`, `trojan://`, `ss://`, `hysteria2://`/`hy2://`, `tuic://`, base64 lists, and sing-box JSON subscriptions.
- "Auto" mode picks the server with the lowest latency using sing-box `urltest`, rechecked every 2 minutes. Servers in Russia are left out, because they don't help with geo-blocked services.
- Per-server ping. When disconnected it measures TCP connect time; when connected it measures the real delay through the tunnel.
- Three modes:
  - **TUN**: all system traffic, like Hiddify's VPN mode.
  - **System proxy**: sets the KDE and GNOME proxy settings, and puts them back if another program resets them.
  - **Local port only**: HTTP and SOCKS5 on `127.0.0.1:12400`.
- Russian sites go direct: `.ru`/`.su`/`.рф` plus the `geoip-ru` and `geosite-category-ru` rule sets.
- Tray icon, live speed, session traffic, exit IP with a country flag, and core logs.
- Themes: **purple** (the default) and **paper**, which is off-white with blue ruled lines and a red margin.

## Build

```sh
# Arch: gtk3 json-glib libsoup3 libayatana-appindicator sing-box
make
sudo make install          # /usr/local/bin/wellide + .desktop + icon
```

## TUN without root

Settings → "Grant TUN permissions" runs `pkexec` once. It copies `sing-box` to `/usr/local/lib/wellide/sing-box` (mode `0750`, group = your group) and gives it `cap_net_admin,cap_net_raw,cap_net_bind_service`. Wellide itself never runs as root.

To stop KDE asking for a password on every connect (sing-box configures DNS through systemd-resolved), add a polkit rule like `/etc/polkit-1/rules.d/49-wellide.rules`. It allows only the `org.freedesktop.resolve1.set-*`/`revert` actions, only for active local sessions of `wheel`.

## CLI

The first instance keeps running; later calls are forwarded to it:

```
wellide --import 'https://sub.example/…'   # add a subscription
wellide --connect | --disconnect | --toggle
wellide --status                            # on/off, profile, server
wellide --hidden                            # start in the tray
wellide --quit
```

## Files

- `~/.config/wellide/settings.ini`, `profiles.json`, `profiles/<id>.txt`: raw subscription bodies, `0600`. They are parsed again on every start.
- `~/.cache/wellide/config.json`: the generated sing-box config. Also `*.srs` rule sets.

## Known limitations

- The TUN interface is IPv4-only (`172.19.0.1/30`, DNS `ipv4_only`). Most VPN exits can't carry IPv6, and when the TUN had an IPv6 address, TLS failed with `unexpected eof`.
- On a TUN, `stack: gvisor` is used because firewalld's default zone rejects TCP on unknown interfaces.
- `xhttp`, `kcp` and `quic` transports aren't supported by sing-box, so links using them are skipped.
