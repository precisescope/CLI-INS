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
- IPv4 only (see below)

## Features

- **Wireless ADB pairing** with a six-digit code
- **Connect and disconnect** saved devices, with one ADB status check per screen
- **Saved devices** stored as friendly names with IPv4 addresses
- **File manager** for browsing folders and installing APKs
  - Folders first, sorted alphabetically, with file sizes
  - Symlinks are followed: a link to a folder opens it (marked `@`)
  - Broken links and special files are shown only in "all files" mode, and cannot be opened
  - Toggle hidden files (`.`) and non-APK files (`a`)
  - Jump to any path with `g` (`~` is your Termux home) or go home with `h`
  - Go up with Backspace or `u`; the folder you came from stays selected
  - Enter on an APK installs it on the device you choose
- **Themes** from the main menu: seven presets, or a custom mix of text, background, and accent colors
  - Changes apply immediately and are saved between sessions

## Keys

- Arrow keys move, Enter selects, Esc or `q` goes back
- Esc and `q` never confirm an action. Confirmation prompts default to **No**, and you have to pick the yes option.

## Requirements and limits

- **IPv4 addresses only.** Hostnames (`phone.local`) and IPv6 addresses (including link-local `fe80::…%wlan0`) are rejected. Wireless debugging shows an IPv4 address on the pairing screen, so that is what the app asks for. Accepting IPv6 would need scope-ID handling, and hostnames would need DNS or mDNS. Both can be added later if there is demand.
- **Android 11 or newer** with Wireless debugging enabled.
- **Terminal size:** at least 40 columns by 14 rows.
- **Shared storage:** run `termux-setup-storage` for the file manager. Without it, the file manager opens your Termux home folder instead.
- **Timeouts:** the app does not time out `adb` commands. If `adb connect` hangs, the screen waits until `adb` returns. Cancelling from the keyboard is not possible while `adb` is running.
- **Pairing code visibility:** the six-digit code is passed to `adb pair` as a command-line argument, so it is briefly visible to other processes on the device through `ps`. It is wiped from the app's memory right after `adb` finishes.

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
5. Run it:
```
./cli-ins
```

## Configuration

CLI-INS stores its files in `~/.config/cli-ins/`:

- `devices.tsv`: saved device names and IPv4 addresses, one per line, tab-separated
- `theme.conf`: your selected theme

Both files are written atomically: the app writes a temporary file, flushes it to disk, then renames it into place. If a save fails, the app says so. A theme that failed to save still applies for the current session.

**Invalid phonebook lines** (wrong format, non-IPv4 hosts, duplicates, or lines longer than 500 characters, which no valid entry can be) are ignored when the file loads, and the app reports how many it skipped. They are dropped the next time the phonebook is saved. Back up `devices.tsv` first if you want to keep them for manual editing.

## Testing

The automated tests drive the real program in a pseudo-terminal with a fake `adb`. They need Python 3 and `pyte` (a terminal emulator library):

```
pip install pyte
CLI_INS_BIN=./cli-ins python3 tests/tests.py
```

[TEST_CHECKLIST.md](TEST_CHECKLIST.md) lists what the automated tests cover and what still needs a real Android device.

## License

MIT License.

Made by precisescope.
