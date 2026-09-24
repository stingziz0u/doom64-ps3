# tools/mk_wess.py -- agrega al Makefile el grupo de source/wess (el audio
# original del N64). Se corre una vez desde la raiz del proyecto:
#   python3 tools/mk_wess.py
import re, sys
p = 'Makefile'
s = open(p).read()
if 'wess.o' in s:
    print('Makefile ya tenia el WESS'); sys.exit(0)
s, n = re.subn(r'^(vpath %\.c [^\n]*)$', r'\1 source/wess', s, count=1, flags=re.M)
assert n == 1, 'no encontre la linea vpath'
api = ['Init', 'Ready', 'Render48k', 'Render22k', 'ChunkSize', 'ActiveVoices',
       'StartSound', 'Trigger', 'Stop', 'StopType', 'StopAll', 'Status',
       'PauseAll', 'ResumeAll', 'SetSfxVolume', 'SetMusVolume']
block = '''# Audio original del N64: el WESS (driver de musica/efectos de Williams, del
# DOOM64-RE) + n64synth.c (el sintetizador de libultra en C). Los datos
# (doom64.wmd/wsd/wdd) los saca de la ROM ps3_romconv.c. Igual que el
# conversor: un solo objeto con `ld -r`, global solo PS3_Wess_*.
WESS_SRCS := $(notdir $(wildcard source/wess/*.c))
WESS_OBJS := $(WESS_SRCS:.c=.o)
WESS_API  := ''' + ' '.join('PS3_Wess_' + a for a in api) + '''
OBJS += wess.o

$(WESS_OBJS): CFLAGS += -w -funsigned-char -fno-strict-aliasing -Isource/wess

wess.o: $(WESS_OBJS)
\tppu-ld -r -o wess_all.o $(WESS_OBJS)
\tppu-objcopy $(foreach s,$(WESS_API),--keep-global-symbol=$(s)) wess_all.o $@

'''
m = re.search(r'^\$\(TARGET\)\.elf:', s, flags=re.M)
assert m, 'no encontre la regla $(TARGET).elf'
s = s[:m.start()] + block + s[m.start():]
open(p, 'w').write(s)
print('Makefile actualizado (WESS)')
