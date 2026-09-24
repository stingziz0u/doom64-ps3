#----------------------------------------------------------------------------
# Doom64Cell -- port de Doom 64 a PS3 homebrew (PSL1GHT)
#
# Etapa 3a: el renderer del motor dibuja en el RSX a traves de un emulador
# del TA del PowerVR (compat/ps3_pvr.c). Geometria sin texturas.
#
#   make            -> .pkg (igual que TyrQuakeCell / CrispyCell)
#   make clean
#   make check-toolchain
#
# IMPORTANTE: el empaquetado (.elf -> .self -> .pkg) lo hace enteramente
# ppu_rules con sus pattern rules propias. NO hacerlo a mano: la version
# hecha a mano se saltea el strip + sprxlinker antes de firmar y el
# package_finalize despues de pkg.py, y el resultado instala pero no corre.
# Esto ya costo una vuelta entera en TyrQuake.
#----------------------------------------------------------------------------

ifeq ($(strip $(PS3DEV)),)
$(error PS3DEV no esta seteado. Hace: export PS3DEV=/usr/local/ps3dev)
endif

include $(PS3DEV)/ppu_rules

#----------------------------------------------------------------------------
# Identidad del paquete
#
# APPID: EXACTAMENTE 9 caracteres. Con 8 la PS3 rellena a 9 con "_" y todas
# las rutas construidas con el dejan de coincidir. Alfabetico puro funciona.
#
# ':=' y no '?=': ppu_rules define TITLE y APPID con '?=' ANTES de estas
# lineas, asi que con '?=' ganarian sus defaults ("Untitled PSL1GHT homebrew"
# + icono generico).
#----------------------------------------------------------------------------
TARGET    := doom64cell
TITLE     := Doom 64
APPID     := DOOM64CEL
CONTENTID := UP0001-$(APPID)_00-0000000000000000

PKGFILES  := $(CURDIR)/pkgfiles
ICON0     := $(CURDIR)/ICON0.PNG

CC := ppu-gcc
LD := ppu-gcc

#----------------------------------------------------------------------------
# Flags
#
# LIBPSL1GHT_INC / LIBPSL1GHT_LIB los exporta ppu_rules -- usarlos en vez de
# armar los -I/-L a mano es lo que evita que una libreria se ignore en
# silencio.
#----------------------------------------------------------------------------
# -std=gnu11: doom64-dc usa gnu17, pero ppu-gcc 7.2 llega hasta gnu11 (gnu17
# aparecio en gcc 8). Tiene que ser gnu y no c, porque el motor usa literales
# binarios (0b1010), que son extension GNU.
CFLAGS += -O2 -Wall -Wno-unused-variable -std=gnu11
CFLAGS += -D__PS3__ -DPS3_APPID=\"$(APPID)\"
# compat/ va PRIMERO: el motor hace #include <kos.h> y tiene que encontrar
# nuestro shim, no el de KallistiOS (que no existe aca).
CFLAGS += -Isource -Isource/compat -Isource/engine
CFLAGS += $(LIBPSL1GHT_INC)

# ps3_video.c va a -O1 -fno-inline a proposito: los syscall stubs de PSL1GHT
# son simbolos de indireccion de datos, no funciones normales, y el inlining
# agresivo rompe su resolucion en link (aparecen/desaparecen undefined
# references a videoGetState segun si el init queda inlineado o no).
CFLAGS_VIDEO := $(filter-out -O2,$(CFLAGS)) -O1 -fno-inline

# Combinacion confirmada funcionando en hardware real (misma que TyrQuakeCell).
# -lrt y -llv2 son OBLIGATORIAS: sin ellas el binario linkea sin errores, el
# PKG instala bien, y al abrirlo da pantalla negra y vuelta al XMB sin llegar
# nunca a main(). Cero logs.
LIBS := gcm_sys rsx sysutil io audio rt lv2 m

#----------------------------------------------------------------------------
# Fuentes
#----------------------------------------------------------------------------
vpath %.c source source/compat source/engine source/romconv source/wess

# Capa de plataforma (nuestra)
SRCS := main.c ps3_log.c ps3_pad.c ps3_video.c ps3_rsx.c ps3_wad.c ps3_map.c ps3_data.c \
        ps3_engine.c

# Shim de KallistiOS -> PS3
SRCS += ps3_kos_stub.c ps3_platform_stub.c ps3_engine_glue.c ps3_w_wad.c \
        ps3_game.c ps3_matrix.c ps3_pvr.c ps3_audio.c ps3_system.c

# Motor original, SIN PARCHES. Entran 54 de los 57 archivos.
#
# Los 3 que quedan afuera son la capa de plataforma, que es justamente lo que
# hay que reescribir para PS3:
#   w_wad.c    carga texturas a la VRAM del PVR   -> source/compat/ps3_w_wad.c
#   i_main.c   video, input, VMU, loop de frames  -> source/compat/ps3_platform_stub.c
#   sndwav.c   streaming ADPCM (KOS)              -> source/compat/ps3_audio.c
ENGINE_SRCS := am_main.c bc5_decoder.c c_convert.c d_main.c d_screens.c decodes.c \
                dll.c doominfo.c doomlib.c f_main.c g_game.c hash.c in_main.c m_bbox.c \
                m_fixed.c m_main.c m_password.c md5c.c p_base.c p_ceilng.c p_change.c \
                p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_macros.c p_map.c \
                p_maputl.c p_misc.c p_mobj.c p_move.c p_plats.c p_pspr.c p_setup.c \
                p_shoot.c p_sight.c p_slide.c p_spec.c p_switch.c p_telept.c p_tick.c \
                p_user.c r_data.c r_lights.c r_main.c r_phase1.c r_phase2.c r_phase3.c \
                s_sound.c sprinfo.c st_main.c tables.c z_zone.c

SRCS += $(ENGINE_SRCS)
OBJS := $(SRCS:.c=.o)

ENGINE_OBJS := $(ENGINE_SRCS:.c=.o)

#----------------------------------------------------------------------------
# Reglas
#
# $(TARGET).self y $(TARGET).pkg NO se definen aca: vienen de las pattern
# rules propias de ppu_rules (%.self: %.elf y %.pkg: %.self), que ademas
# levantan $(PKGFILES) y el ICON0 solas.
#----------------------------------------------------------------------------
.PHONY: all clean check-toolchain getlog

# El PKG que se instala es el .gnpdrm.pkg (el que pasa por package_finalize),
# no el .pkg pelado. ppu_rules genera los dos; el pelado es intermedio.
#
#   make           -> doom64-ps3-1.0.pkg  (release)
#   make DEBUG=1   -> doom64debug.pkg     (con los botones de prueba en Gamepad)
# Al cambiar entre uno y otro, siempre 'make clean' antes.
VERSION := 1.0
ifeq ($(DEBUG),1)
OUT_PKG := doom64debug.pkg
CFLAGS  += -DPS3_TEST_BUTTONS
else
OUT_PKG := doom64-ps3-$(VERSION).pkg
endif

all: $(TARGET).pkg
	@cp $(TARGET).gnpdrm.pkg $(OUT_PKG)
	@echo ""
	@echo "PKG a instalar: $(CURDIR)/$(OUT_PKG)"
	@ls -la $(OUT_PKG)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# El motor es codigo ajeno que compila con avisos propios del estilo de 1997.
# Se silencian para que en la salida solo se vean los avisos de NUESTRO
# codigo, que son los que importan.
$(ENGINE_OBJS): CFLAGS += -w -include stdint.h

# Regla explicita para ps3_video.o (ver CFLAGS_VIDEO arriba).
ps3_video.o: source/ps3_video.c
	$(CC) $(CFLAGS_VIDEO) -c $< -o $@

# ps3_rsx.c toca GCM igual que ps3_video.c: mismo tratamiento, por las dudas.
ps3_rsx.o: source/ps3_rsx.c
	$(CC) $(CFLAGS_VIDEO) -c $< -o $@

# Conversor de la ROM (el wadtool de doom64-dc, adaptado a big-endian): corre
# en el primer arranque, ver source/ps3_romconv.c. Sus simbolos (DecodeD64,
# lumpinfo, SwapShort...) chocan con los del motor, asi que se juntan en un
# solo objeto con `ld -r` y se deja global unicamente PS3_RomConv_*.
# -fsigned-char: el wadtool se escribio para x86 (char con signo).
RC_SRCS := rc_decodes.c rc_encode.c rc_imgproc.c rc_mapconv.c rc_wadtool.c
RC_OBJS := $(RC_SRCS:.c=.o)
OBJS += ps3_romconv.o romconv.o

$(RC_OBJS): CFLAGS += -w -fsigned-char -include source/romconv/rc_port.h -Isource/romconv

romconv.o: $(RC_OBJS)
	ppu-ld -r -o romconv_all.o $(RC_OBJS)
	ppu-objcopy --keep-global-symbol=PS3_RomConv_Rom --keep-global-symbol=PS3_RomConv_Lost \
	    romconv_all.o $@

# Audio original del N64: el WESS (driver de musica/efectos de Williams, del
# DOOM64-RE) + n64synth.c (el sintetizador de libultra en C). Los datos
# (doom64.wmd/wsd/wdd) los saca de la ROM ps3_romconv.c. Igual que el
# conversor: un solo objeto con `ld -r`, global solo PS3_Wess_*.
WESS_SRCS := $(notdir $(wildcard source/wess/*.c))
WESS_OBJS := $(WESS_SRCS:.c=.o)
WESS_API  := PS3_Wess_Init PS3_Wess_Ready PS3_Wess_Render48k PS3_Wess_Render22k PS3_Wess_ChunkSize PS3_Wess_ActiveVoices PS3_Wess_StartSound PS3_Wess_Trigger PS3_Wess_Stop PS3_Wess_StopType PS3_Wess_StopAll PS3_Wess_Status PS3_Wess_PauseAll PS3_Wess_ResumeAll PS3_Wess_SetSfxVolume PS3_Wess_SetMusVolume
OBJS += wess.o

$(WESS_OBJS): CFLAGS += -w -funsigned-char -fno-strict-aliasing -Isource/wess

wess.o: $(WESS_OBJS)
	ppu-ld -r -o wess_all.o $(WESS_OBJS)
	ppu-objcopy $(foreach s,$(WESS_API),--keep-global-symbol=$(s)) wess_all.o $@

$(TARGET).elf: $(OBJS)
	$(LD) $(OBJS) $(LIBPSL1GHT_LIB) -o $(TARGET).elf $(foreach lib,$(LIBS),-l$(lib))

#----------------------------------------------------------------------------
# Directorio de contenido del PKG e icono
#
# pkgfiles/ replica la estructura del PKG, NO su USRDIR: ppu_rules hace
#   cp -rf $(PKGFILES)/* $(BUILDDIR)/pkg/
# asi que lo que tenga que terminar en el USRDIR va en pkgfiles/USRDIR/.
# (Mismo patron que TyrQuakeCell con pkgfiles/USRDIR/id1/music/.)
#
# Los datos del juego NO van aca: se suben por FTP directo al USRDIR de la
# consola, asi no hay que reempaquetar 19 MB en cada vuelta.
#
# OJO: el directorio no puede quedar VACIO. El guard de ppu_rules chequea
# -n y -d, asi que un directorio existente pero vacio lo pasa, y despues
# 'cp -rf $(PKGFILES)/*' falla porque el glob no expande -> "cannot stat".
# Por eso siempre se escribe VERSION.TXT, que ademas sirve para confirmar en
# la consola que build quedo instalado.
#----------------------------------------------------------------------------
$(PKGFILES)/USRDIR/VERSION.TXT:
	@mkdir -p $(PKGFILES)/USRDIR
	@echo "$(TITLE) -- v1.0 -- `date +%Y-%m-%d\ %H:%M`" > $@

# El icono del XMB (320x176, PNG con alfa real). Si falta, se genera uno
# liso para no frenar el build.
$(ICON0):
	@echo "No hay ICON0.PNG, generando un placeholder 320x176..."
	@python3 -c "import zlib,struct;w,h=320,176;\
rows=b''.join(b'\x00'+bytes([40,20,25,255])*w for _ in range(h));\
png=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d));\
open('$(ICON0)','wb').write(b'\x89PNG\r\n\x1a\n'+png(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+png(b'IDAT',zlib.compress(rows))+png(b'IEND',b''))"

$(TARGET).pkg: $(PKGFILES)/USRDIR/VERSION.TXT $(ICON0)

#----------------------------------------------------------------------------
# Diagnostico
#----------------------------------------------------------------------------
check-toolchain:
	@echo "PS3DEV   = $(PS3DEV)"
	@echo "ppu_rules: `test -f $(PS3DEV)/ppu_rules && echo OK || echo FALTA`"
	@$(CC) --version | head -1
	@echo ""
	@echo "APPID    = $(APPID)  (longitud: `printf '%s' '$(APPID)' | wc -c`, TIENE que ser 9)"
	@echo "TITLE    = $(TITLE)"
	@echo ""
	@echo "Librerias en $(PS3DEV)/ppu/lib:"
	@echo "  (NO usar 'ppu-gcc -print-file-name': ignora -L y da falsos negativos)"
	@for l in $(LIBS); do \
	   if [ -f "$(PS3DEV)/ppu/lib/lib$$l.a" ]; then \
	     printf "  OK    lib%s.a\n" "$$l"; \
	   elif $(CC) -print-file-name=lib$$l.a | grep -q /; then \
	     printf "  OK    lib%s.a (sysroot de gcc)\n" "$$l"; \
	   else \
	     printf "  FALTA lib%s.a\n" "$$l"; \
	   fi; \
	 done
	@echo ""
	@echo "LIBPSL1GHT_INC = $(LIBPSL1GHT_INC)"
	@echo "LIBPSL1GHT_LIB = $(LIBPSL1GHT_LIB)"

# Opcional: traer el log sin salir de la terminal.
#   make getlog PS3IP=192.168.1.50
PS3USER ?= anonymous:anonymous
getlog:
	@test -n "$(PS3IP)" || (echo "Usa: make getlog PS3IP=<ip>"; exit 1)
	curl -s -u $(PS3USER) \
	     ftp://$(PS3IP)/dev_hdd0/game/$(APPID)/USRDIR/doom64_log.txt \
	     -o doom64_log.txt
	@echo "--- doom64_log.txt ---"
	@cat doom64_log.txt

clean:
	rm -f *.o $(TARGET).elf $(TARGET).self $(TARGET).fake.self
	rm -f $(PKGFILES)/USRDIR/VERSION.TXT
	rm -rf build
	@echo "limpio (el .pkg no se toca: ppu_rules lo regenera solo)"
