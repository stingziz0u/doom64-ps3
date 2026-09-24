/* compat/dc/maple.h -- bus Maple del Dreamcast. En PS3 es ioPad (etapa 2). */
#ifndef PS3_COMPAT_DC_MAPLE_H
#define PS3_COMPAT_DC_MAPLE_H
#include <stdint.h>

#define MAPLE_FUNC_CONTROLLER  0x01000000
#define MAPLE_FUNC_MEMCARD     0x02000000
#define MAPLE_FUNC_KEYBOARD    0x40000000
#define MAPLE_FUNC_MOUSE       0x08000000
#define MAPLE_FUNC_PURUPURU    0x00010000

typedef struct { int port, unit, valid; uint32_t functions; void *status; } maple_device_t;

maple_device_t *maple_enum_type(int n, uint32_t func);
maple_device_t *maple_enum_dev(int p, int u);
int             maple_dev_status(maple_device_t *dev);
#endif
