#!/bin/bash
# tools/hosttest/build.sh -- corre el motor ENTERO en x86 (Linux/WSL) y guarda
# frames renderizados por software en out/*.ppm.
#
# Sirve para probar el emulador del TA, las texturas y la logica del juego sin
# la consola: el codigo es el mismo, solo cambia el backend final (ps3_rsx.c
# por soft_rsx.c, que rasteriza en CPU con las mismas reglas).
#
# Necesita los datos del juego en /dev_hdd0/game/DOOM64CEL/USRDIR (la misma
# ruta que en la PS3):
#   sudo mkdir -p /dev_hdd0/game/DOOM64CEL /dev_hdd0/data/doom64
#   sudo cp -r <carpeta con pow2.wad, alt.wad, maps/, tex/...> /dev_hdd0/game/DOOM64CEL/USRDIR
#   sudo chown -R $USER /dev_hdd0
#
# Uso:  ./build.sh && ./hosttest
# El log queda en /dev_hdd0/game/DOOM64CEL/USRDIR/doom64_log.txt, igual que en la consola.
set -e
cd "$(dirname "$0")"
R=../..
SRCS="$R/source/ps3_log.c $R/source/ps3_wad.c $R/source/ps3_map.c $R/source/ps3_data.c \
      $R/source/ps3_engine.c $R/source/compat/ps3_kos_stub.c $R/source/compat/ps3_platform_stub.c \
      $R/source/compat/ps3_engine_glue.c $R/source/compat/ps3_w_wad.c $R/source/compat/ps3_game.c \
      $R/source/compat/ps3_matrix.c $R/source/compat/ps3_pvr.c"
for f in $R/source/engine/*.c; do
  case "$(basename $f)" in w_wad.c|i_main.c|s_sound.c|sndwav.c) ;; *) SRCS="$SRCS $f";; esac
done
mkdir -p out
gcc -std=gnu11 -O2 -g -w -include stdint.h -I$R/source -I$R/source/compat -I$R/source/engine \
    -DPS3_APPID='"DOOM64CEL"' -o hosttest hostmain.c soft_rsx.c $SRCS -lm
echo "listo: ./hosttest  (frames en out/*.ppm)"
