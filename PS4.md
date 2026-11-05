# Diddy Kong Racing on PS4

A native PS4 version of [Golden Balloon](README.md), the Diddy Kong Racing
source port. It is not an emulator: the game runs as a normal PS4 app, in
1080p widescreen.

**No game files are included.** You need your own copy of the original game
(a ROM file).

## What you need

1. **A PS4 with GoldHEN.** Tested on an original PS4 ("fat" model). A PS4 Pro
   or Slim should work too, but has not been tested.
2. **A USB stick**, formatted as **exFAT** or **FAT32**.
3. **Your Diddy Kong Racing ROM**:
   - the **US** or the **European** version, **Rev A** (also called "1.1"),
   - exactly **12 MB** (12,582,912 bytes),
   - file ending `.z64`, `.v64` or `.n64`. The name does not matter.

   Not sure? Check your file with the
   [ROM checker](https://akratch.github.io/golden-balloon/rom-check.html).
   It runs in your browser; nothing is uploaded.
4. **Two PS4 system files** for the graphics: `libScePigletv2VSH.sprx` and
   `libSceShaccVSH.sprx`.
   - If Ship of Harkinian or the Super Mario 64 port already runs on your
     PS4, you already have them. Skip this point.
   - Otherwise: they are Sony files and cannot be shared here. Ports that use
     the same graphics library explain where to get them.

## Install

1. Download `DKR-xx.yy.pkg` from the
   [Releases page](../../releases/latest).
2. Copy these files onto the **top level** of the USB stick (not into a
   folder):
   - `DKR-xx.yy.pkg`
   - your ROM file
   - the two `.sprx` files (only if you need them, see above)
3. Plug the stick into the PS4. Make sure GoldHEN is running.
4. Open **Settings → Debug Settings → Game → Package Installer**, choose
   `DKR-xx.yy.pkg` and install it.
5. Start **Diddy Kong Racing** from the home screen. Leave the stick plugged
   in for the first start: the game copies the ROM (and the `.sprx` files, if
   they are on the stick) and shows a short message while it does so.

After the first start you no longer need the stick.

## Controls

Use one DualShock 4 controller. The game is for **one player**; split-screen
multiplayer is not supported yet.

## Your save data

Saves are stored on the PS4 in `/data/dkr/save/`. They stay there when you
update or reinstall the game.

## Update to a new version

Install the new `.pkg` the same way. The PS4 says the game is already
installed and asks whether to install it again: choose **Yes**. Your saves and
your ROM stay in place.

## If something goes wrong

The game shows a message in the top-left corner of the screen.

| Message | What to do |
|---|---|
| *no ROM found* | Put your ROM on the top level of the USB stick and start again. |
| *could not copy the ROM from the USB stick* | Try another stick, or format it as exFAT. |
| *Check the ROM* | Your ROM is not a supported version. It must be US or European **Rev A**, 12 MB. Use the ROM checker linked above. |
| *libScePigletv2VSH.sprx and libSceShaccVSH.sprx are missing* | Put the two `.sprx` files on the top level of the stick and start again. |

The game also writes a log file to `/data/dkr/ps4_boot.log`. If you report a
problem, please attach it (for example via the GoldHEN FTP server).

## Remove the game

Delete it from the home screen like any other app. To also remove the ROM and
your saves, delete the folder `/data/dkr` (for example with a file manager
like PS4Xplorer).

## For developers

How to build the package yourself: [platform/ps4/README.md](platform/ps4/README.md).

## Credits

- [Golden Balloon](https://github.com/akratch/goldenballoon) by its
  contributors: the source port this build is based on (MIT licence).
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
- The PS4 ports of Super Mario 64 and Ship of Harkinian, which showed how to
  use Sony's OpenGL ES library (Piglet) from homebrew.

Diddy Kong Racing is a trademark of Nintendo. This project is not affiliated
with or endorsed by Nintendo or Rare.
