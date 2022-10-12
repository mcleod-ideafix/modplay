# modplay
A basic, yet comprehensive mod player (as in AMIGA Protracker). Written in portable C code with audio code for both Win32 and DOS (Sound Blaster) systems.

## Compilation
- Windows (MinGW-32): run make -f Makefile-mingw32
- DOS (Open Watcom C): run WMAKE -f MAKEFILE.MK1 mplay.exe. Target is a Causeway 32-bit executable, 386 minimum to execute. No 80x87 needed.

## Prerequisites for DOS build
- Sound Blaster or register 100% compatible sound card present and initialized (run CTCM.EXE if your card is a jumperless ISA card).
- ISA DMA subsystem present.
- A correctly set up BLASTER environment variable (CTCM.EXE sets a valid one). Currently, only DMA 1 and 3 are supported.

## Limitations
- Some effects are not yet supported. Hopefully, rarely used ones.
- Fails to detect non supported MOD files. Anything that it's not a M.K. or FLT4 mod files, are interpreted as 15 instrument mods.
- Output is 8 bit mono, to be able to use a Sound Blaster 2.0 card as a minimum.

## Use
- modplay nameofyourfavouritemod[.MOD]
- The slowest system I have tested this in is a 80386DX-33 based PC with Sound Blaster Pro 2.
