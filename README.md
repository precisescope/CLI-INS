CLI-INS

Android APK Installation Manager for Termux

It's just a really, really convenient way to install APKs through ADB without dealing with the extra waiting and friction introduced by Android's installation restrictions.

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

What does it do?

- Wireless ADB pairing
- Connect to Android devices
- Save devices in a phonebook
- Browse folders and APK files
- Install APKs directly through ADB

Installation

Requires Termux, ADB, and the necessary runtime dependencies.

1. Install the required packages:

pkg update
pkg install git clang ncurses android-tools

2. Clone the repository:

git clone https://github.com/precisescope/CLI-INS.git

3. Enter the project directory:

cd CLI-INS

4. Run the precompiled executable:

./cli-ins

Compiling (if your phone and Termux want to remove 2 minutes from your lifespan)

1. Clone the repository:

git clone https://github.com/precisescope/CLI-INS.git

2. Enter the project directory:

cd CLI-INS

3. Compile:

clang -std=c11 -Wall -Wextra -O2 cli-ins.c -o cli-ins -lncurses

4. Make it executable:

chmod +x cli-ins

5. Run it:

./cli-ins

License

MIT License.

Made by precisescope.
