# Dependencies

CLI-INS is a C program that runs in Termux. It needs the following packages:

| Package | Purpose |
|---|---|
| clang | Compile the C source |
| ncurses | Terminal user interface |
| android-tools | ADB pairing, connection and APK installation |
| git | Clone the repository |

## Install dependencies

```bash
pkg update && pkg upgrade
pkg install clang ncurses android-tools git
```

## Android setup

1. Enable **Developer options** on your device.
2. Enable **Wireless debugging** (Android 11 or newer). This is required for pairing and connecting.
3. Allow Termux access to shared storage. The file manager browses `/storage/emulated/0`:

```bash
termux-setup-storage
```

If storage access is not granted, the file manager falls back to your Termux home folder.

## Build from source

```bash
clang -std=c11 -Wall -Wextra -O2 cli-ins.c -o cli-ins -lncurses
```

