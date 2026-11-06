#!/bin/sh
# Builds tools/rrq_unpack against the reference ALIS engine sources (msys2 mingw64 gcc + SDL2).
set -e
M=/c/Users/Msi/Tools/rrq_coop
A=$M/reference/alis/src
export PATH=/c/msys64/mingw64/bin:$PATH
SRCS=$(ls $A/*.c $A/sys/sys.c $A/sys/sys_sdl2.c $A/audio/*.c $A/render/*.c $A/video/*.c $A/emu2149/emu2149.c $A/emu8950/emu8950.c $A/emu8950/emuadpcm.c | grep -v '/main.c$' | grep -v atari | grep -v dsp_mixer)
mkdir -p $M/build/rrq_unpack
gcc -O1 -w -o $M/build/rrq_unpack/rrq_unpack.exe $M/tools/rrq_unpack/rrq_unpack.c $SRCS \
  -I$A -I$A/sys -I$A/audio -I$A/render -I$A/video -I$A/emu2149 -I$A/emu8950 \
  $(sdl2-config --cflags) $(sdl2-config --libs | sed 's/-mwindows//') -lm
cp /c/msys64/mingw64/bin/SDL2.dll $M/build/rrq_unpack/
