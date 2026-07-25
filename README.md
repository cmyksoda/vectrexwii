vectrexwii
==========

VectrexWii is a Wii port of Vecx, a Vectrex emulator written by Valavan Manohararajah.

## Features

- Many games included: All of the original Vectrex library (except for the 3-D Imager and Lightpen games), two prototype games, and lots of mods and demos from the Vectrex homebrew community.
- Overlays: Overlays included for all official games, prototypes, and mods of official games.
- Sound support: Finally implemented after over a decade.

## BYOR (Bring your own ROM support)

Before the project was abandoned, this was the only way to play ROMs. It is still supported. Add `.vec` files to a folder called `vec` at the root of your SD card, and the emulator will detect them. Use the SD option in the cartridge select to view your files. For overlays, make sure they have the exact same name as your ROM, and are a 388x480px PNG in the RGBA color format.

## Building from source

If you don't want to use the included boot.dol on the releases page, you can build from source using this repo. GRRLIB and devkitPro are required.

## Wiibrew page
Wiki info is available [here](http://wiibrew.org/wiki/VectrexWii). *Not updated at the time of writing*
