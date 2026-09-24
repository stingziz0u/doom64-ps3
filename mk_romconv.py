import re, sys
p = 'Makefile'
s = open(p).read()
if 'romconv.o' in s:
    print('Makefile ya tenia el conversor'); sys.exit(0)
s, n = re.subn(r'^(vpath %\.c [^\n]*)$', r'\1 source/romconv', s, count=1, flags=re.M)
assert n == 1, 'no encontre la linea vpath'
block = '''# Conversor de la ROM (el wadtool de doom64-dc, adaptado a big-endian): corre
# en el primer arranque, ver source/ps3_romconv.c. Sus simbolos (DecodeD64,
# lumpinfo, SwapShort...) chocan con los del motor, asi que se juntan en un
# solo objeto con `ld -r` y se deja global unicamente PS3_RomConv_*.
# -fsigned-char: el wadtool se escribio para x86 (char con signo).
RC_SRCS := rc_decodes.c rc_encode.c rc_imgproc.c rc_mapconv.c rc_wadtool.c
RC_OBJS := $(RC_SRCS:.c=.o)
OBJS += ps3_romconv.o romconv.o

$(RC_OBJS): CFLAGS += -w -fsigned-char -include source/romconv/rc_port.h -Isource/romconv

romconv.o: $(RC_OBJS)
\tppu-ld -r -o romconv_all.o $(RC_OBJS)
\tppu-objcopy --keep-global-symbol=PS3_RomConv_Rom --keep-global-symbol=PS3_RomConv_Lost \\
\t    romconv_all.o $@

'''
m = re.search(r'^\$\(TARGET\)\.elf:', s, flags=re.M)
assert m, 'no encontre la regla $(TARGET).elf'
s = s[:m.start()] + block + s[m.start():]
open(p, 'w').write(s)
print('Makefile actualizado')
