/* ps3_pad.h -- DualShock 3 via ioPad.
 *
 * Layout de paddata.button[], confirmado contra TyrQuakeCell:
 *   [2],[3]  digitales, bitmask de 16 bits partido en dos bytes
 *   [4],[5]  stick derecho X, Y   (0-255, 128 centrado)
 *   [6],[7]  stick izquierdo X, Y
 */

#ifndef PS3_PAD_H
#define PS3_PAD_H

#include <ppu-types.h>

#define PAD_LEFT      0x8000
#define PAD_DOWN      0x4000
#define PAD_RIGHT     0x2000
#define PAD_UP        0x1000
#define PAD_START     0x0800
#define PAD_R3        0x0400
#define PAD_L3        0x0200
#define PAD_SELECT    0x0100
#define PAD_SQUARE    0x0080
#define PAD_CROSS     0x0040
#define PAD_CIRCLE    0x0020
#define PAD_TRIANGLE  0x0010
#define PAD_R1        0x0008
#define PAD_L1        0x0004
#define PAD_R2        0x0002
#define PAD_L2        0x0001

int  PS3_Pad_Init(void);
void PS3_Pad_Shutdown(void);

/* Llamar una vez por frame antes de consultar el estado. */
void PS3_Pad_Frame(void);

u16  PS3_Pad_Buttons(void);              /* estado actual (level) */
int  PS3_Pad_Pressed(u16 mask);          /* flanco de bajada->subida */
int  PS3_Pad_Connected(void);

/* Sticks ya con deadzone aplicada, rango -127..127 */
int  PS3_Pad_LeftX(void);
int  PS3_Pad_LeftY(void);
int  PS3_Pad_RightX(void);
int  PS3_Pad_RightY(void);

void PS3_Pad_SetDeadzone(int dz);             /* 0..100 sobre 128 */
void PS3_Pad_Rumble(int small_on, int big_level);

#endif /* PS3_PAD_H */
