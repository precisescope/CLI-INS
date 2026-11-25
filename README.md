# CLI-INS

cli apk installer for Termux

It's just a really, really fast af way to install apks via adb without googles bullshit update

Here's some pros and cons of it ;)

Pros (⁻u⁻)

- gud
- fast as hell
- easy af
- open source
- low storage usage
- made on Earth by an actual human

Cons (⁻n⁻)

- vibecoders might not understand most of what's happening
- might be a horror story for people LARPing as power users
- requires ADB and a compatible Android/Termux environment

## Features

- **Wireless ADB pairing** with a six-digit code
- **Connect and disconnect** saved devices
- **Saved devices phonebook** with friendly names
- **File manager** for browsing folders and installing APKs
  - Folders first, sorted alphabetically, with file sizes
  - Toggle hidden files (`.`) and non-APK files (`a`)
  - Jump to any path with `g` (`~` is your Termux home) or go home with `h`
  - Go up with Backspace or `u`; the folder you came from stays selected
  - Enter on an APK installs it on the device you choose
- **Themes** from the main menu: seven presets, or a custom mix of text, background, and accent colors
  - Changes apply immediately and are saved between sessions

## Installation

Requires Termux, ADB, and the runtime dependencies. See [DEPENDENCIES.md](DEPENDENCIES.md) for the full list and Android setup steps.

1. Install the required packages:
```
pkg update
pkg install git clang ncurses android-tools
```
2. Clone the repository:
```
git clone https://github.com/precisescope/CLI-INS.git
```
3. Enter the project directory:
```
cd CLI-INS
```
4. Compile:
```
clang -std=c11 -Wall -Wextra -O2 cli-ins.c -o cli-ins -lncurses
```
5. Make it executable:
```
chmod +x cli-ins
```
6. Run it:
```
./cli-ins
```

## Configuration

CLI-INS stores its files in `~/.config/cli-ins/`:

- `devices.tsv`: saved device names and IP addresses
- `theme.conf`: your selected theme

## License

MIT License.

Made by precisescope.

