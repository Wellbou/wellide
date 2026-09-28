<p align="center"><img src="docs/banner.svg" alt="wellide" width="100%"></p>

<p align="center">
  <b>A tiny VPN client on top of <a href="https://github.com/SagerNet/sing-box">sing-box</a>.</b><br>
  Written in C and GTK. Runs on Linux and Windows. Uses about 125 MB of RAM with the core included.<br>
  <a href="#русский">Русский ↓</a>
</p>

---

## Install

**Linux** (Arch, Debian 12+, Ubuntu 22.04+, Fedora, openSUSE, Alpine):

```sh
curl -fsSL https://raw.githubusercontent.com/Wellbou/wellide/main/install.sh | sh
```

The script installs the build dependencies and sing-box, builds Wellide and gives TUN permissions to a private copy of the core. Wellide itself never runs as root. Pass `sh -s -- --no-tun` to skip the TUN step and `--uninstall` to remove everything.

**Windows 10/11:** get `wellide-x.y.z-setup.exe` from [Releases](https://github.com/Wellbou/wellide/releases). It already includes sing-box and wintun.

## What it does

<p align="center"><img src="docs/button.svg" alt="vortex button" width="480"></p>

- **One button.** Click the pixel vortex. It winds up when you connect and unwinds when you disconnect.
- **Subscriptions and keys:** `https://` subscriptions (with traffic and expiry info), `vless:// vmess:// trojan:// ss:// hy2:// tuic://`. Leave the field empty to paste from the clipboard. `wellide://import/<url>` deep links also work.
- **Auto** picks the server with the lowest ping and re-checks every 2 min. It skips servers located in your own country, because those can't get past geo-blocks.
- **Automatic ping.** Offline it measures TCP connect time; when connected it measures the real delay through each server.
- **Modes:** TUN (all traffic), system proxy (KDE, GNOME, Windows), or a local port only (`127.0.0.1:12400`, HTTP and SOCKS5 on the same port).
- **Local sites go direct.** Optional split routing for 🇷🇺 Russia, 🇮🇷 Iran and 🇨🇳 China, using sing-geoip/geosite rule sets.
- **Live traffic graph** and tray icon. English and Russian are picked from the system locale.

## Themes

<p align="center"><img src="docs/themes.svg" alt="themes" width="100%"></p>

**Void** is the default: black and violet with pixel edges. **Notebook** is ruled paper with ink outlines. There are also **Purple** and **Graphite**.

### Custom themes

To add a theme, drop an `.ini` file into `~/.config/wellide/themes/` (on Windows, `%LOCALAPPDATA%\wellide\themes\`). The app applies it as soon as you save, without a restart. Settings → *Custom themes…* opens that folder.

```ini
[theme]
name=Midnight
base=void            ; inherit everything you don't set
bg=#101018           ; window
bg2=#16161f          ; sidebar
card=#1d1d29
fg=#e8e8f0
fg-dim=#8a8aa0
accent=#7c4dff       ; buttons, active tab
accent-fg=#ffffff
line=#2a2a3a
danger=#ff5277
ink=#2a2240          ; dark pixels of the vortex
glow=#7c4dff         ; bright pixels of the vortex
pixel=true           ; square, stepped borders
ruled=false          ; notebook ruling
outline=false        ; ink outlines + offset shadows
radius=0

[css]
extra=.h1 { letter-spacing: 2px; }
file=midnight.css    ; any GTK3 CSS, loaded after the theme
```

## Memory

<p align="center"><img src="docs/ram.svg" alt="RAM" width="560"></p>

This is RSS measured on Arch and KDE while connected in TUN mode with a 20-server subscription.

## Command line

```
wellide [--hidden] [--import URL] [--connect|--disconnect|--toggle] [--status] [--quit]
```

When Wellide is already running, these commands are sent to that window. You can use them in scripts and hotkeys.

## Build

```sh
# deps: gtk3 json-glib libsoup3 (+ libayatana-appindicator for the tray)
make && sudo make install
python3 tools/art.py   # regenerate icons and README art
```

Windows builds run in GitHub Actions: MSYS2 UCRT64 for the app, NSIS for the installer. See `.github/workflows/build.yml`.

---

## Русский

**Лёгкий VPN-клиент на sing-box.** Написан на C и GTK, работает на Linux и Windows. Вместе с ядром занимает около 125 МБ памяти.

### Установка

Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/Wellbou/wellide/main/install.sh | sh
```

Скрипт ставит зависимости и sing-box, собирает Wellide и выдаёт права для TUN отдельной копии ядра. Сам Wellide root-права не получает.

Windows: скачайте установщик `wellide-x.y.z-setup.exe` из [Releases](https://github.com/Wellbou/wellide/releases).

### Возможности

- **Одна кнопка — вихрь.** При подключении пиксели закручиваются в вихрь, и надпись меняется с «ВКЛ» на «ВЫКЛ».
- **Подписки и ключи.** Поддерживаются подписки с остатком трафика и сроком, а также ключи vless, vmess, trojan, ss, hy2 и tuic. Если поле пустое, ссылка берётся из буфера обмена.
- **«Авто»** выбирает сервер с лучшим пингом и не берёт серверы вашей страны.
- **Пинг обновляется сам**, нажимать ничего не нужно.
- **Режимы:** TUN (весь трафик), системный прокси или только локальный порт.
- **Сайты своей страны напрямую:** Россия, Иран, Китай.
- **Темы.** По умолчанию Void, есть «Тетрадь», Purple и Graphite. Свои темы — это `.ini`-файлы в `~/.config/wellide/themes/`. Формат описан выше, изменения применяются сразу.

## License

MIT
