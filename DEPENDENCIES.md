
# Dependencies

CLI-INS requires the following packages on Termux:

| Package | Purpose |
|---|---|
| clang | Compile the C source |
| ncurses | Terminal user interface |
| android-tools | ADB pairing, connection and APK installation |
| git | Clone and manage the repository |

## Install dependencies

```bash
pkg update && pkg upgrade
pkg install clang ncurses android-tools git
```

## Android setup

Enable Developer Options and Wireless Debugging on your Android device.

Allow Termux storage access:

```bash
termux-setup-storage
```

