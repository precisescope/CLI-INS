
# CLI-INS

**CLI Installer for Android — v1.1**

A lightweight terminal-based Android installer built in C with ncurses for Termux. also includes a precompiled executeable use it (compile if it doesn't) 

Created by **precisescope**.

Instagram: **@8bit_binary**

## Features

- Wireless ADB device pairing
- Connect to Android devices
- Save devices in a local phonebook
- Browse Android storage
- Select APK files using keyboard navigation
- Install APKs through ADB
- not needing bullshit default package manager :) 

## Installation

Clone the repository:

```bash
git clone https://github.com/precisescope/CLI-INS.git
cd CLI-INS
```

Install dependencies:

```bash
pkg install clang ncurses android-tools git
```

Compile:

```bash
clang -Wall -Wextra -O2 cli-ins.c -o cli-ins -lncurses
```

Run:

```bash
./cli-ins
```

## Requirements

- Android device running Termux
- Wireless Debugging enabled for ADB pairing
- ADB-compatible Android device
- Required Termux packages

## License

This project is licensed under the MIT License.
See [LICENSE](LICENSE).

