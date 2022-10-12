# modplay
A basic, yet comprehensive mod player (as in AMIGA Protracker). Written in portable C code with audio code for both Win32 and DOS (Sound Blaster) systems.

## Compilation
- Windows (MinGW-32): run make -f Makefile-mingw32
- DOS (Open Watcom C): run WMAKE -f MAKEFILE.MK1 mplay.exe. Target is a Causeway 32-bit executable, 386 minimum to execute. No 80x87 needed.

## Prerequisites for DOS build
- Sound Blaster or register 100% compatible sound card present and initialized (run CTCM.EXE if your card is a jumperless ISA card).
- ISA DMA subsystem present.
- A correctly set up BLASTER environment variable (CTCM.EXE sets a valid one). Currently, only DMA 1 and 3 are supported.

## Use
- modplay nameofyourfavouritemod[.MOD]
