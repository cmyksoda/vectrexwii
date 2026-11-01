VectrexWii
==========

VectrexWii is a Wii port of Vecx, a Vectrex emulator written by Valavan Manohararajah.  

Originally ported by community member Aruskano, and currently maintained by myself, this project seeks to bring as much of the charm of the Vectrex to the Wii as possible.

## Features

- Many games included: Almost all of the original Vectrex library (currently missing the 3-D Imager and Lightpen games, intentionally missing the licensed titles for legal reasons), two prototype games, and plenty of mods and demos from the Vectrex homebrew community.
- Overlays: Overlays included for all official games, prototypes, and mods of official games.
- Sound support: Finally implemented after over a decade.

## BYOR (Bring your own ROM support)

Before the project was abandoned, this was the only way to play ROMs. It is still supported. Add `.vec` files to a folder called `vec` at the root of your SD card, and the emulator will detect them. Use the SD option in the cartridge select to view your files. For overlays, make sure they have the exact same name as your ROM, and are a 388x480px PNG in the RGBA color format.

## Building from source

If you don't want to use the included boot.dol on the releases page, you can build from source using this repo. GRRLIB and devkitPro are required.

## Wiibrew page
Wiki info is available [here](http://wiibrew.org/wiki/VectrexWii).

---

*This project was made with AI assistance. For more information, see [my AI usage statement](https://github.com/cmyksoda/cmyksoda/blob/main/AI_USAGE.md).*

## More controllers

On top of the Wii Remote (with Nunchuk and Classic Controller) and GameCube pads the program already reads, these work with no mapping of their own, using the same buttons as the Wii Remote:

- **USB mouse, pen tablet or uDraw GameTablet.** Left click, pen tip or uDraw pen is A, right click or the lower pen switch / rocker is B, and middle click or the upper pen switch / rocker is 2. While one is in use it replaces the Wii Remote's pointer where the program has one. Wacom, Huion, XP-Pen, Gaomon and similar tablets work; the uDraw plugs into any Wii Remote.
- **GameCube controller adapter for Wii U** (and third-party ones that copy it): its four ports read as GameCube pads, rumble aside.
- **DualShock 3 / Sixaxis** over USB, **Wii U Pro Controller** and **Wii U GamePad** (vWii): Cross is A, Circle is B, Triangle is 2, Square is 1, Start / R1 / R2 is +, Select / L1 / L2 is -, PS is Home, and the left stick presses the D-pad. These need their libraries, which `scripts/build_controller_portlibs.sh` builds into `./portlibs/ppc` (run it once before `make`); without them the program builds as before, minus those three.

The drivers are from [libwiiinput](https://github.com/quatric/libwiiinput) and live in `source/libwiiinput/`; `extrainput_compat.h` wraps the program's own `WPAD_*` and `PAD_*` calls, so no input code in the program had to change. The GameCube adapter driver is GPL-3.0-or-later (from RiftWii) and the rest AGPL-3.0, which are the terms of the files in that folder.
