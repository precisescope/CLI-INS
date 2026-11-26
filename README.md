# CLI-INS

cli based apk installer for android :p
It's just a really, really convenient way to install APKs through ADB without dealing with package manager crashing unexpectedly

Here's some pros and cons of it ;)

Pros (⁻u⁻)

- gud
- fast as hell
- easy af
- open source
- low storage usage
- wireless ADB pairing
- saves devices in a phonebook
- built-in APK file browser
- custom themes
- made on Earth by an actual human

Cons (⁻n⁻)

- vibecoders might not understand most of what's happening
- might be a horror story for people LARPing as power users
- requires ADB and a compatible Android/Termux environment
- it does not look like its from the cold war era

What does it do?

- Wireless ADB pairing using the six-digit pairing code
- Connect and disconnect saved Android devices
- Save device names and IP addresses in a local phonebook
- Browse folders and APK files
- Install APKs directly through ADB
- Five built-in themes and custom colour combinations
- Update CLI-INS directly from GitHub
- Keyboard-controlled terminal interface

Installation

Install the dependencies:
```
pkg update
pkg install git clang ncurses android-tools
Give Termux storage access:
```
```
termux-setup-storage
```
Clone the repository:

```
git clone https://github.com/precisescope/CLI-INS.git
cd CLI-INS
```
Compiling (cause it's quick af and also safer than using a precompiled binary)
```
clang -std=c11 -Wall -Wextra -O2 cli-ins.c -o cli-ins -lncurses
chmod +x cli-ins
./cli-ins
```
Requirements

- Termux
- Android 11 or newer
- Wireless debugging enabled
- ADB (android-tools)
- ncurses
- Git and Clang for compiling and updating

Things to know

- IPv4 addresses only. Hostnames and IPv6 aren't supported.
- Minimum terminal size is 40 columns by 20 rows.
- Run "termux-setup-storage" to access shared Android storage.
- Pairing codes are temporarily passed to ADB as command-line arguments.
- Device names, IP addresses and themes are stored in "~/.config/cli-ins/".
- Update files are stored in "~/.cache/cli-ins/".
- The updater builds the latest code from the main branch. No precompiled binaries are downloaded.
- The updater does not perform signature verification.

License

MIT License.

Made by precisescope.
