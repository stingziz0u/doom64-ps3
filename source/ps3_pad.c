/* ps3_pad.c -- DualShock 3 via ioPad.
 *
 * Deteccion de flanco incluida desde el arranque: el throttle tipico de los
 * motores (joywait) solo limita la repeticion a ~7 por segundo, asi que
 * mantener un boton apretado atraviesa varios menus. Las direcciones SI
 * quedan level-triggered a proposito (repetir es lo que uno quiere al
 * scrollear una lista).
 *
 * El puerto activo se re-resuelve cada frame: una reconexion USB/BT puede
 * caer en un puerto distinto al anterior.
 */

#include <string.h>
#include <io/pad.h>

#include "ps3_pad.h"
#include "ps3_log.h"

#define STICK_CENTER    128
static int s_deadzone = 24;     /* ~24 sobre 128; Opciones > Gamepad */
#define NUM_PAD_PORTS   7       /* literal a proposito: MAX_PADS no es portable */

static padInfo2 s_info;
static padData  s_data;

static int s_connected = 0;
static int s_port      = -1;    /* -1 = ninguno */

static u16 s_buttons  = 0;
static u16 s_prev     = 0;

static int s_lx = 0, s_ly = 0, s_rx = 0, s_ry = 0;

static int s_fail   = 0;        /* ioPadGetData fallando seguido */
static int s_nopad  = 0;        /* frames sin ningun mando */
static int s_rumble_reset = 0;  /* avisa a PS3_Pad_Rumble que olvide su estado */

static int apply_deadzone(int raw)
{
    int v = raw - STICK_CENTER;
    if (v > -s_deadzone && v < s_deadzone) return 0;
    /* reescalar para que el recorrido util siga llegando a +-127 */
    if (v > 0) v = (v - s_deadzone) * 127 / (127 - s_deadzone);
    else       v = (v + s_deadzone) * 128 / (128 - s_deadzone);
    return v;
}

static void release_all(void)
{
    s_buttons = 0;
    s_lx = s_ly = s_rx = s_ry = 0;
    /* s_data se relee aunque un poll venga con len==0: dejarlo "en reposo"
     * (sticks al centro, nada apretado), no en cero (cero = stick a fondo). */
    memset(&s_data, 0, sizeof(s_data));
    s_data.button[4] = s_data.button[5] = STICK_CENTER;
    s_data.button[6] = s_data.button[7] = STICK_CENTER;
}

int PS3_Pad_Init(void)
{
    s32 ret = ioPadInit(7);
    ps3_logf("[pad] ioPadInit ret=%d", (int)ret);
    if (ret < 0) return -1;

    memset(&s_info, 0, sizeof(s_info));
    release_all();
    s_prev = 0;
    s_port = -1;
    s_connected = 0;
    return 0;
}

void PS3_Pad_Shutdown(void)
{
    ioPadEnd();
}

/* Ultimo recurso: reiniciar la libreria de pads (lo mismo que hace el port
 * de ECWolf al cerrar y reabrir el joystick de SDL cuando queda colgado). */
static void pad_reinit(const char *why)
{
    s32 a = ioPadEnd();
    s32 b = ioPadInit(7);
    ps3_logf("[pad] reinicio de ioPad (%s): end=%d init=%d", why, (int)a, (int)b);
    s_port = -1;
    s_fail = 0;
    s_nopad = 0;
    s_rumble_reset = 1;
    release_all();
}

void PS3_Pad_Frame(void)
{
    static u32 last_status[MAX_PORT_NUM];
    static int info_calls = 0;
    int i, port = -1;
    s32 ri;

    s_prev = s_buttons;

    /* ioPadGetInfo2 (no la vieja ioPadGetInfo): trae el estado por PUERTO,
     * bit 0 = conectado, bit 1 = "cambio la asignacion" (el mando se apago
     * y volvio, o cambio de puerto). */
    ri = ioPadGetInfo2(&s_info);
    info_calls++;
    if (info_calls <= 1)
        ps3_logf("[pad] info %d: ret=%d conectados=%u info=0x%x status=%x %x %x %x",
                 info_calls, (int)ri, (unsigned)s_info.connected, (unsigned)s_info.info,
                 (unsigned)s_info.port_status[0], (unsigned)s_info.port_status[1],
                 (unsigned)s_info.port_status[2], (unsigned)s_info.port_status[3]);
    if (ri < 0) {
        if (s_connected) ps3_logf("[pad] ioPadGetInfo2 fallo (%d): mando perdido", (int)ri);
        s_connected = 0;
        release_all();
        if (++s_nopad == 300) pad_reinit("GetInfo2 fallando");
        return;
    }

    /* Log de cada cambio de estado de cada puerto: asi se ve en el log
     * exactamente que pasa al apagar y prender el mando. */
    for (i = 0; i < MAX_PORT_NUM; i++) {
        if (s_info.port_status[i] != last_status[i]) {
            ps3_logf("[pad] puerto %d: status 0x%x -> 0x%x (tipo %u, capacidad 0x%x)",
                     i, (unsigned)last_status[i], (unsigned)s_info.port_status[i],
                     (unsigned)s_info.device_type[i], (unsigned)s_info.device_capability[i]);
            last_status[i] = s_info.port_status[i];
        }
        /* Cambio de asignacion: vaciar el buffer viejo de ese puerto. */
        if (s_info.port_status[i] & 2)
            ioPadClearBuf(i);
    }

    /* Puerto activo: se mantiene el actual mientras siga conectado; si no,
     * el primer mando conectado (una reconexion puede caer en otro puerto). */
    if (s_port >= 0 && (s_info.port_status[s_port] & 1))
        port = s_port;
    else
        for (i = 0; i < MAX_PORT_NUM; i++)
            if (s_info.port_status[i] & 1) { port = i; break; }

    if (port < 0) {
        if (s_connected) ps3_logf("[pad] sin mandos conectados");
        s_connected = 0;
        s_port = -1;
        release_all();
        s_nopad++;
        return;
    }

    if (!s_connected || port != s_port) {
        ps3_logf("[pad] mando activo en puerto %d (antes %d)%s", port, s_port,
                 s_nopad > 0 ? " -- reconectado" : "");
        ioPadClearBuf(port);
        release_all();
        s_rumble_reset = 1;
        s_fail = 0;
    }
    s_port = port;
    s_connected = 1;
    s_nopad = 0;

    {
        s32 r = ioPadGetData(s_port, &s_data);
        static int calls = 0, logged_first = 0;
        calls++;
        if (calls <= 1)
            ps3_logf("[pad] poll %d: ret=%d puerto=%d status=0x%x len=%d botones=%02x%02x",
                     calls, (int)r, s_port, (unsigned)s_info.port_status[s_port],
                     (int)s_data.len, (unsigned)s_data.button[2], (unsigned)s_data.button[3]);
        if (r != 0) {
            if (s_fail < 5) ps3_logf("[pad] ioPadGetData(%d) = %d", s_port, (int)r);
            /* Colgado varios segundos con el mando "conectado": reiniciar. */
            if (++s_fail == 180) pad_reinit("GetData fallando");
            return;     /* conservar el estado anterior, no soltar los botones */
        }
        if (s_fail) ps3_logf("[pad] GetData volvio a andar tras %d fallos", s_fail);
        s_fail = 0;
        if (!logged_first && ((s_data.button[2] | s_data.button[3]) & 0xff)) {
            logged_first = 1;
            ps3_logf("[pad] primer boton recibido (poll %d): %02x%02x", calls,
                     (unsigned)s_data.button[2], (unsigned)s_data.button[3]);
        }
    }

    /* OJO: NO hacer bail si len==0. s_data conserva los valores del poll
     * anterior, y saltear la lectura congela el cursor a 60fps. */

    s_buttons = (u16)((s_data.button[2] << 8) | s_data.button[3]);

    s_rx = apply_deadzone(s_data.button[4]);
    s_ry = apply_deadzone(s_data.button[5]);
    s_lx = apply_deadzone(s_data.button[6]);
    s_ly = apply_deadzone(s_data.button[7]);
}

u16 PS3_Pad_Buttons(void)   { return s_buttons; }
int PS3_Pad_Connected(void) { return s_connected; }

int PS3_Pad_Pressed(u16 mask)
{
    return (s_buttons & mask) && !(s_prev & mask);
}

int PS3_Pad_LeftX(void)  { return s_lx; }
int PS3_Pad_LeftY(void)  { return s_ly; }
int PS3_Pad_RightX(void) { return s_rx; }
int PS3_Pad_RightY(void) { return s_ry; }

void PS3_Pad_SetDeadzone(int dz)
{
    if (dz < 0) dz = 0;
    if (dz > 100) dz = 100;
    s_deadzone = dz;
}

/* Vibracion del DS3: motor chico on/off, motor grande 0..255. */
void PS3_Pad_Rumble(int small_on, int big_level)
{
    static int last_small = -1, last_big = -1;
    padActParam p;
    if (!s_connected || s_port < 0) return;
    if (s_rumble_reset) { s_rumble_reset = 0; last_small = last_big = -1; }
    if (last_small < 0 && !small_on && !big_level) return;   /* nunca vibro: no tocar */
    if (small_on == last_small && big_level == last_big) return;
    last_small = small_on;
    last_big = big_level;
    memset(&p, 0, sizeof(p));
    p.small_motor = small_on ? 1 : 0;
    p.large_motor = (u8)(big_level < 0 ? 0 : (big_level > 255 ? 255 : big_level));
    ioPadSetActDirect(s_port, &p);
}
