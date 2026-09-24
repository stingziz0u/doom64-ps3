/* ps3_system.c -- la capa de plataforma "de verdad" (lo que en el port de
 * Dreamcast es i_main.c): el flujo completo del juego corre con el codigo
 * original del motor (D_DoomMain -> titulo, demos, menus, G_RunGame,
 * intermission, final) y aca se implementa lo que ese codigo le pide a la
 * plataforma:
 *
 *   I_GetControllerData   DS3 -> botones del N64 (juego / menu), trucos,
 *                         temporizacion de frames, salida al XMB
 *   I_WIPE_*              transiciones melt / fade (leen el framebuffer)
 *   I_*Pak*               saves y configuracion en /dev_hdd0/data/doom64
 *   I_Rumble & cia.       vibracion del DS3
 *   PS3_ControlPad*       pantalla "Gamepad" del menu de opciones
 *
 * Todo lo que dependa de la consola (pad, video, salida) va por las
 * funciones PS3_Plat_* (main.c en la PS3, hostmain.c en x86), asi este
 * archivo se prueba entero en tools/hosttest. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "doomdef.h"
#include "p_local.h"
#include "st_main.h"
#include "../ps3_rsx.h"
#include "ps3_log.h"
#include "ps3_game.h"

#ifndef PS3_DATADIR
#define PS3_DATADIR "/dev_hdd0/data/doom64"
#endif

/* ------------------------------------------------------------------ */
/* Plataforma (main.c / hostmain.c)                                    */
/* ------------------------------------------------------------------ */
void     PS3_Plat_PollInput(ps3_input_t *in);
int      PS3_Plat_ExitRequested(void);
void     PS3_Plat_Exit(void) __attribute__((noreturn));
uint64_t PS3_Plat_TimeNs(void);
void     PS3_Plat_Rumble(int small_on, int big_level);
int      PS3_Plat_Capture565(uint16_t *dst);   /* 320x240 en filas de 512 */
void     PS3_Plat_SetDeadzone(int dz);

/* Motor */
void D_SplashScreen(void);
int  D_TitleMap(void);
int  D_RunDemo(char *name, skill_t skill, int map);
int  D_Credits(void);
int  M_RunTitle(void);
void M_ResetSettings(doom64_settings_t *s);
void P_RefreshBrightness(void);
void P_ExitLevel(void);
void P_FlushAllCached(void);
extern boolean run_hectic_demo;
extern int early_error;
extern int last_Ltrig, last_Rtrig;
extern int MenuAnimationTic, cursorpos, text_alpha;
extern int ticon, gametic;

/* ------------------------------------------------------------------ */
/* Tiempo (KOS)                                                        */
/* ------------------------------------------------------------------ */
uint64_t perf_cntr_timer_ns(void) { return PS3_Plat_TimeNs(); }
int PS3_Sys_FpsUncap(void) { return global_render_state.fps_uncap; }
uint32_t rtc_unix_secs(void)      { return (uint32_t)(PS3_Plat_TimeNs() / 1000000000ull); }

/* ================================================================== */
/* Configuracion del mando (pantalla "Gamepad")                        */
/* ================================================================== */
#define PAD_CFG_FILE PS3_DATADIR "/ps3pad.cfg"

typedef struct {
    int layout;         /* 0 = clasico, 1 = alternativo */
    int deadzone;       /* 4..48 sobre 128 */
    int cheats;         /* L3 / R3 / triangulo */
} padcfg_t;

/* Los botones de prueba solo existen en el build de debug (make DEBUG=1).
 * En el release quedan apagados y no aparecen en la pantalla Gamepad; los
 * trucos estan en el menu Features. */
#ifdef PS3_TEST_BUTTONS
static padcfg_t s_pad = { 0, 24, 1 };
#else
static padcfg_t s_pad = { 0, 24, 0 };
#endif

static void padcfg_save(void)
{
    FILE *f = fopen(PAD_CFG_FILE, "w");
    if (!f) { ps3_logf("[cfg] no se pudo escribir %s", PAD_CFG_FILE); return; }
    fprintf(f, "layout=%d\ndeadzone=%d\ncheats=%d\n", s_pad.layout, s_pad.deadzone, s_pad.cheats);
    fclose(f);
    ps3_logf("[cfg] mando guardado: layout=%d deadzone=%d trucos=%d",
             s_pad.layout, s_pad.deadzone, s_pad.cheats);
}

static void padcfg_load(void)
{
    char line[64];
    FILE *f = fopen(PAD_CFG_FILE, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            int v;
            if (sscanf(line, "layout=%d", &v) == 1)   s_pad.layout = (v == 1);
            if (sscanf(line, "deadzone=%d", &v) == 1) s_pad.deadzone = v;
            if (sscanf(line, "cheats=%d", &v) == 1)   s_pad.cheats = (v != 0);
        }
        fclose(f);
    }
#ifndef PS3_TEST_BUTTONS
    s_pad.cheats = 0;
#endif
    if (s_pad.deadzone < 4)  s_pad.deadzone = 4;
    if (s_pad.deadzone > 48) s_pad.deadzone = 48;
    PS3_Plat_SetDeadzone(s_pad.deadzone);
    ps3_logf("[cfg] mando: layout=%s deadzone=%d trucos=%s (%s)",
             s_pad.layout ? "moderno" : "clasico", s_pad.deadzone,
             s_pad.cheats ? "si" : "no", f ? "archivo" : "por defecto");
}

/* ================================================================== */
/* Vibracion                                                           */
/* ================================================================== */
/* El motor arma un "paquete" por evento (I_GetDamageRumble) y lo manda con
 * I_Rumble. Formato propio: bits 0-7 = duracion en frames, 8-15 = fuerza
 * del motor grande (0-255), bit 16 = motor chico. */
int rumble_patterns[NUM_RUMBLE];
static int      s_rumble_on = 0;
static uint64_t s_rumble_until = 0;

#define RPKT(frames, level, small) ((frames) | ((level) << 8) | ((small) << 16))

void I_InitRumble(i_rumble_pak_t pak)
{
    /* Intensidades a ojo, por evento (enum i_rumble_t de doomdef.h). */
    rumble_patterns[rumble_hoof]      = RPKT(8, 120, 0);
    rumble_patterns[rumble_quake]     = RPKT(30, 90, 0);
    rumble_patterns[rumble_punch]     = RPKT(5, 110, 0);
    rumble_patterns[rumble_saw]       = RPKT(4, 60, 1);
    rumble_patterns[rumble_sawready]  = RPKT(3, 30, 1);
    rumble_patterns[rumble_missile]   = RPKT(8, 170, 0);
    rumble_patterns[rumble_bfg]       = RPKT(20, 230, 1);
    rumble_patterns[rumble_plasma]    = RPKT(3, 60, 0);
    rumble_patterns[rumble_pistol]    = RPKT(3, 70, 0);
    rumble_patterns[rumble_shotgun]   = RPKT(6, 150, 0);
    rumble_patterns[rumble_shotgun2]  = RPKT(8, 200, 0);
    rumble_patterns[rumble_cgun]      = RPKT(3, 90, 0);
    rumble_patterns[rumble_laser]     = RPKT(6, 140, 1);
    rumble_patterns[rumble_oof]       = RPKT(5, 80, 0);
    rumble_patterns[rumble_thunder]   = RPKT(25, 110, 0);

    s_rumble_on = (pak != 0);
    ps3_logf("[rumble] vibracion %s (nivel %d)", s_rumble_on ? "activada" : "apagada", (int)pak);
    if (!s_rumble_on) PS3_Plat_Rumble(0, 0);
}

int I_GetDamageRumble(int damage)
{
    int level = 80 + damage * 4, frames = 6 + damage / 4;
    if (level > 255) level = 255;
    if (frames > 30) frames = 30;
    return (frames & 0xff) | (level << 8) | (damage >= 30 ? 0x10000 : 0);
}

void I_Rumble(uint32_t packet)
{
    int frames = packet & 0xff, level = (packet >> 8) & 0xff, small = (packet >> 16) & 1;
    if (!s_rumble_on || demoplayback) return;
    if (frames == 0) frames = 6;
    s_rumble_until = PS3_Plat_TimeNs() + (uint64_t)frames * 16683334ull;
    PS3_Plat_Rumble(small, level);
}

static void rumble_update(void)
{
    if (s_rumble_until && PS3_Plat_TimeNs() >= s_rumble_until) {
        s_rumble_until = 0;
        PS3_Plat_Rumble(0, 0);
    }
}

/* ================================================================== */
/* Mando -> botones del N64                                            */
/* ================================================================== */
static int clamp127(int v) { return v > 127 ? 127 : (v < -127 ? -127 : v); }

static unsigned short s_game_prev = 0;

static int build_game_buttons(const ps3_input_t *in)
{
    unsigned short d = in->buttons;
    int b = 0;

    if (d & DS3_START) b |= PAD_START;
    if (d & DS3_UP)    b |= PAD_UP;
    if (d & DS3_DOWN)  b |= PAD_DOWN;
    if (d & DS3_LEFT)  b |= PAD_LEFT;
    if (d & DS3_RIGHT) b |= PAD_RIGHT;

    if (s_pad.layout == 0) {                       /* clasico */
        if (d & (DS3_R2 | DS3_SQUARE)) b |= PAD_Z_TRIG;    /* disparar */
        if (d & DS3_CROSS)             b |= PAD_RIGHT_C;   /* usar */
        if (d & DS3_L1)                b |= PAD_A;         /* arma anterior */
        if (d & DS3_R1)                b |= PAD_B;         /* arma siguiente */
        if (d & DS3_SELECT)            b |= PAD_UP_C;      /* automapa */
        if (d & DS3_L2)                b |= PAD_LEFT_C;    /* caminar */
    } else {                                       /* moderno */
        /* Con los botones de prueba activos, L2 es el modificador de los
         * trucos (L2+L3, L2+R3, L2+triangulo): mientras se aprieta, L3 y
         * triangulo no hacen su accion normal. */
        static int run_toggle = 0;
        unsigned short pressed = d & ~s_game_prev;
        int mod = s_pad.cheats && (d & DS3_L2);
        if (d & DS3_R2)                   b |= PAD_Z_TRIG;    /* disparar */
        if (d & (DS3_SQUARE | DS3_CROSS)) b |= PAD_RIGHT_C;   /* usar */
        if (d & DS3_L1)                   b |= PAD_A;         /* arma anterior */
        if (d & DS3_R1)                   b |= PAD_B;         /* arma siguiente */
        if (!mod && (d & DS3_TRIANGLE))   b |= PAD_B;         /* arma siguiente */
        if (d & DS3_SELECT)               b |= PAD_UP_C;      /* automapa */
        if (!mod && (pressed & DS3_L3)) {                     /* correr (toggle) */
            player_t *p = &players[0];
            run_toggle = !run_toggle;
            if (p->mo && !demoplayback)
                p->message = (run_toggle ^ (menu_settings.Autorun != 0)) ? "Run ON" : "Run OFF";
            S_StartSound(NULL, sfx_switch1);
        }
        if (run_toggle)                   b |= PAD_LEFT_C;
    }
    s_game_prev = d;

    /* Stick izquierdo X: strafe proporcional (el motor lee last_L/Rtrig). */
    last_Ltrig = last_Rtrig = 0;
    if (in->lx < 0) {
        b |= PAD_L_TRIG;
        last_Ltrig = (-in->lx * 2 > 255) ? 255 : -in->lx * 2;
    } else if (in->lx > 0) {
        b |= PAD_R_TRIG;
        last_Rtrig = (in->lx * 2 > 255) ? 255 : in->lx * 2;
    }
    /* Caminar/correr. En el motor BT_SPEED (L2/L1 aca) solo cambia la
     * velocidad de la cruceta y del strafe; el stick analogico de adelante
     * siempre usa la velocidad maxima (en el N64 se caminaba empujando
     * menos el stick). Asi que para el stick se aplica aca: correr =
     * boton XOR autorun, igual que P_BuildMove; caminando, el eje se
     * escala por forwardmove[0]/forwardmove[1] = 0xE000/0x16000 = 7/11. */
    {
        int fwd = clamp127(-in->ly);
        int run = ((b & PAD_LEFT_C) != 0) ^ (menu_settings.Autorun != 0);
        if (!run) fwd = fwd * 7 / 11;
        b |= fwd & 0xff;                           /* adelante/atras */
    }
    b |= (clamp127(in->rx) & 0xff) << 8;           /* giro */
    return b;
}

/* En los menus: cruceta (o stick) para moverse y cambiar valores con
 * izquierda/derecha, X para entrar/elegir, circulo (o START) para volver.
 * En la pantalla de password, cuadrado borra la ultima letra. */
void M_PasswordDrawer(void);
int M_SavePakTicker(void);
int M_LoadPakTicker(void);
extern int (*ps3_cur_ticker)(void);
void M_ControllerPakDrawer(void);

static int build_menu_buttons(const ps3_input_t *in)
{
    unsigned short d = in->buttons;
    int b = 0;
    if (d & (DS3_START | DS3_CIRCLE))       b |= PAD_START;
    if ((d & DS3_UP)    || in->ly < -64)    b |= PAD_UP;
    if ((d & DS3_DOWN)  || in->ly >  64)    b |= PAD_DOWN;
    if ((d & DS3_LEFT)  || in->lx < -64)    b |= PAD_LEFT;
    if ((d & DS3_RIGHT) || in->lx >  64)    b |= PAD_RIGHT;
    /* Guardar/cargar partida: el motor espera el "A" del Dreamcast (que
     * en sus defines es PAD_Z_TRIG), solo, sin otro boton. */
    if (ps3_cur_ticker == M_SavePakTicker || ps3_cur_ticker == M_LoadPakTicker) {
        if (d & DS3_CROSS)                  b |= PAD_Z_TRIG;
    } else if (d & DS3_CROSS)               b |= PAD_A;
    if ((d & DS3_SQUARE) && MenuCall == M_PasswordDrawer)
        b |= PAD_RIGHT_C;
    /* Administrador de partidas: borrar = Y+A del Dreamcast. */
    if ((d & DS3_SQUARE) && MenuCall == M_ControllerPakDrawer)
        b |= PAD_B | PAD_Z_TRIG;
    last_Ltrig = last_Rtrig = 0;
    return b;
}

/* ------------------------------------------------------------------ */
/* Trucos para probar: L3 god mode, R3 todas las armas, triangulo termina */
/* el nivel (en el layout moderno, con L2 apretado). Se apagan desde     */
/* Opciones > Gamepad.                                                   */
/* ------------------------------------------------------------------ */
static void do_cheats(unsigned short now, unsigned short before)
{
    player_t *p = &players[0];
    unsigned short pressed = now & ~before;
    int i;

    if (!s_pad.cheats || in_menu || demoplayback || !p->mo || p->playerstate != PST_LIVE)
        return;
    /* Layout moderno: L3 y triangulo son del juego, los trucos van con L2. */
    if (s_pad.layout == 1 && !(now & DS3_L2))
        return;

    if (pressed & DS3_L3) {
        p->cheats ^= CF_GODMODE;
        p->message = (p->cheats & CF_GODMODE) ? "God mode ON" : "God mode OFF";
        ps3_logf("[truco] god mode %s", (p->cheats & CF_GODMODE) ? "ON" : "OFF");
    }
    if (pressed & DS3_R3) {
        p->cheats |= CF_WEAPONS;
        for (i = 0; i < NUMWEAPONS; i++) p->weaponowned[i] = true;
        for (i = 0; i < NUMAMMO; i++) {
            if (!p->backpack) p->maxammo[i] *= 2;
            p->ammo[i] = p->maxammo[i];
        }
        p->backpack = true;
        for (i = 0; i < NUMCARDS; i++) p->cards[i] = true;
        p->artifacts |= 7;                     /* las 3 llaves demonio (Unmaker) */
        p->message = "All weapons, ammo, keys and artifacts";
        S_StartSound(NULL, sfx_switch2);
        ps3_log("[truco] todas las armas, municion y llaves");
    }
    if (pressed & DS3_TRIANGLE) {
        static int last_map_exit = -1;
        if (last_map_exit != gamemap || gameaction == ga_nothing) {
            last_map_exit = gamemap;
            ps3_logf("[truco] completar nivel %d (siguiente: %d)", gamemap, gamemap + 1);
            p->message = "Level complete";
            P_ExitLevel();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Log del estado del juego (para diagnosticar desde el log)            */
/* ------------------------------------------------------------------ */
static void log_state(const ps3_input_t *in, int eb)
{
    static int last_menu = -1, last_map = -1, last_pause = -1, last_demo = -1;
    static int last_btn = 0, last_weap = -1, last_hp = -1, last_kills = -1;
    static void *last_call = NULL;
    static char *last_msg = NULL;
    player_t *p = &players[0];

    if (in_menu != last_menu) { ps3_logf("[estado] in_menu=%d", in_menu); last_menu = in_menu; }
    if (gamepaused != last_pause) { ps3_logf("[estado] pausa=%d", gamepaused); last_pause = gamepaused; }
    if (demoplayback != last_demo) { ps3_logf("[estado] demo=%d", demoplayback); last_demo = demoplayback; }
    if (gamemap != last_map) {
        ps3_logf("[estado] mapa %d (skill %d)", gamemap, gameskill);
        last_map = gamemap;
        last_hp = last_kills = last_weap = -1;
    }
    (void)in; (void)eb; (void)p;
    (void)last_btn; (void)last_weap; (void)last_hp; (void)last_kills;
    (void)last_call; (void)last_msg;
}

/* ------------------------------------------------------------------ */
/* Brillo: el motor solo aplica el cambio si se esta dibujando un nivel   */
/* (DrawerStatus == 1). Desde el menu del titulo no se veia nada hasta    */
/* el nivel siguiente: aca se aplica apenas cambia.                       */
/* ------------------------------------------------------------------ */
extern int DrawerStatus, infraredFactor;
void P_SetLightFactor(int lightfactor);

static void brightness_watch(void)
{
    static int last = -1;
    if (menu_settings.brightness == last) return;
    ps3_logf("[video] brillo %d -> %d (DrawerStatus=%d, nivel %s)", last, menu_settings.brightness,
             DrawerStatus, players[0].mo ? "cargado" : "no");
    if (last >= 0 && players[0].mo && lights && DrawerStatus != 1)
        P_SetLightFactor(infraredFactor > 100 ? infraredFactor : 100);
    last = menu_settings.brightness;
}

/* ------------------------------------------------------------------ */
/* Contador de FPS (Options > Display > FPS Counter), dibujado encima de   */
/* todo desde pvr_scene_finish.                                           */
/* ------------------------------------------------------------------ */
void PS3_Sys_Overlay(void)
{
    static uint64_t t0 = 0;
    static int frames = 0, shown = 0, last_mode = -1;
    uint64_t now = PS3_Plat_TimeNs();
    char buf[16];
    int mode = menu_settings.VmuDisplay;          /* 0 no, 1 arriba izq, 2 arriba der */

    if (mode != last_mode) {
        ps3_logf("[video] contador de FPS: %s", mode == 1 ? "arriba izquierda" : mode == 2 ? "arriba derecha" : "apagado");
        last_mode = mode;
    }
    frames++;
    if (!t0) t0 = now;
    if (now - t0 >= 500000000ull) {
        shown = (int)((uint64_t)frames * 1000000000ull / (now - t0));
        frames = 0;
        t0 = now;
    }
    if (mode < 1 || mode > 2) return;
    sprintf(buf, "%d FPS", shown);
    ST_DrawString(mode == 1 ? 8 : 306 - 13 * (int)strlen(buf), 6, buf, 0xffff40ff, ST_ABOVE_OVL);
}

/* ------------------------------------------------------------------ */
static uint64_t s_last_frame_ns = 0;
static unsigned short s_prev_ds3 = 0;

void PS3_Sys_Quit(void)
{
    extern void PS3_Audio_Shutdown(void);
    ps3_log("[sys] salida pedida: cerrando");
    PS3_Plat_Rumble(0, 0);
    PS3_Audio_Shutdown();
    PS3_Plat_Exit();
}

/* Ultima lectura del mando (sticks ya con deadzone): la pantalla Gamepad
 * la muestra para probar la deadzone. */
static ps3_input_t s_last_in;

int I_GetControllerData(void)
{
    ps3_input_t in;
    uint64_t now;
    int eb;

    if (PS3_Plat_ExitRequested())
        PS3_Sys_Quit();

    /* Temporizacion: en modo 30 fps el motor cuenta vblanks con drawsync1. */
    now = PS3_Plat_TimeNs();
    if (s_last_frame_ns) {
        int v = (int)((now - s_last_frame_ns + 8341667ull) / 16683334ull);
        drawsync1 = v < 1 ? 1 : (v > 4 ? 4 : v);
    } else {
        drawsync1 = 2;
    }
    vsync += drawsync1;
    drawsync2 = vsync;
    s_last_frame_ns = now;

    rumble_update();
    brightness_watch();

    if (menu_settings.version)                /* configuracion ya cargada */
        PS3_PVR_SetGamma(menu_settings.Gamma);    /* no hace nada si no cambio */

    PS3_Plat_PollInput(&in);
    s_last_in = in;
    {
        /* Al cambiar de pantalla (otro MiniLoop), los botones que ya venian
         * apretados se ignoran hasta soltarlos. Si no, el X que cierra las
         * estadisticas llegaba a la pantalla de guardado como un X "nuevo"
         * (ahi X se traduce a otro boton del motor) y guardaba solo. */
        static int (*last_ticker)(void) = NULL;
        static unsigned short held = 0;
        ps3_input_t bin = in;
        if (ps3_cur_ticker != last_ticker) {
            last_ticker = ps3_cur_ticker;
            held = in.buttons;
        }
        held &= in.buttons;
        bin.buttons &= (unsigned short)~held;
        if (in_menu)
            eb = build_menu_buttons(&bin);
        else
            eb = build_game_buttons(&bin);
    }

    do_cheats(in.buttons, s_prev_ds3);
    s_prev_ds3 = in.buttons;
    log_state(&in, eb);
    return eb;
}

/* ================================================================== */
/* Pantalla "Gamepad" (reemplaza la del Dreamcast)                      */
/* ================================================================== */
#ifdef PS3_TEST_BUTTONS
#define PAD_ITEMS 3
#else
#define PAD_ITEMS 2
#endif
static float s_pad_rep = 0.0f;

int PS3_ControlPadTicker(void)
{
    static int last_tic = 0;
    unsigned int buttons = ticbuttons[0] & 0xffff0000;
    unsigned int oldbuttons = oldticbuttons[0] & 0xffff0000;
    unsigned int pressed = buttons & ~oldbuttons;

    if (((int)f_gamevbls < (int)f_gametic) && ((((int)f_gametic) & 3U) == 0)) {
        if (last_tic != (int)f_gametic) {
            last_tic = (int)f_gametic;
            MenuAnimationTic = (MenuAnimationTic + 1) & 7;
        }
    }

    if (pressed & PAD_START) {
        S_StartSound(NULL, sfx_pistol);
        padcfg_save();
        return 8;
    }

    /* arriba/abajo con repeticion */
    if (!(buttons & (PAD_UP | PAD_DOWN))) {
        s_pad_rep = 0.0f;
    } else {
        s_pad_rep -= f_vblsinframe[0];
        if (s_pad_rep <= 0.0f) {
            s_pad_rep = (float)TICRATE / 2;
            if (buttons & PAD_DOWN) cursorpos = (cursorpos + 1) % PAD_ITEMS;
            else                    cursorpos = (cursorpos + PAD_ITEMS - 1) % PAD_ITEMS;
            S_StartSound(NULL, sfx_switch1);
        }
    }

    /* izquierda/derecha o X cambian el valor */
    if (pressed & (PAD_LEFT | PAD_RIGHT | PAD_A)) {
        int dir = (pressed & PAD_LEFT) ? -1 : 1;
        switch (cursorpos) {
        case 0: s_pad.layout = !s_pad.layout; break;
        case 1:
            s_pad.deadzone += dir * 4;
            if (s_pad.deadzone < 4)  s_pad.deadzone = 4;
            if (s_pad.deadzone > 48) s_pad.deadzone = 48;
            PS3_Plat_SetDeadzone(s_pad.deadzone);
            break;
        case 2: s_pad.cheats = !s_pad.cheats; break;
        }
        S_StartSound(NULL, sfx_switch2);
        ps3_logf("[cfg] layout=%d deadzone=%d trucos=%d", s_pad.layout, s_pad.deadzone, s_pad.cheats);
    }
    return 0;
}

void PS3_ControlPadDrawer(void)
{
    static const char *lay[2][7] = {
        { "fire      R2 / square", "use       cross", "weapons   L1 / R1",
          "map       select", "walk/run  L2", "move      left stick", "turn      right stick" },
        { "fire      R2", "use       square / cross", "weapons   L1 / R1 / triangle",
          "map       select", "run       L3 (toggle)", "move      left stick", "turn      right stick" },
    };
    char buf[64];
    int i, y;
    unsigned int col = text_alpha | 0xc0000000;

    ST_DrawString(-1, 20, "Gamepad", col, ST_ABOVE_OVL);

    sprintf(buf, "Layout: %s", s_pad.layout ? "Modern" : "Classic");
    ST_DrawString(62, 50, buf, col, ST_ABOVE_OVL);
    sprintf(buf, "Deadzone: %d%%", (s_pad.deadzone * 100 + 64) / 128);
    ST_DrawString(62, 68, buf, col, ST_ABOVE_OVL);
#ifdef PS3_TEST_BUTTONS
    sprintf(buf, "Test buttons: %s", s_pad.cheats ? "On" : "Off");
    ST_DrawString(62, 86, buf, col, ST_ABOVE_OVL);
#endif

    ST_DrawSymbol(40, 50 + cursorpos * 18 - 1, MenuAnimationTic + 0x46, text_alpha | 0xffffff00, ST_ABOVE_OVL);

    /* Prueba de la deadzone: los sticks tal como le llegan al juego
     * (-100..100). Con el stick suelto tiene que quedar en 0. */
    sprintf(buf, "stick test  L %4d %4d  R %4d %4d",
            s_last_in.lx * 100 / 127, -s_last_in.ly * 100 / 127,
            s_last_in.rx * 100 / 127, -s_last_in.ry * 100 / 127);
    ST_Message(40, 102, buf, text_alpha | 0xffffff00, ST_ABOVE_OVL);

    y = 116;
    for (i = 0; i < 7; i++, y += 11)
        ST_DrawString(40, y, (char *)lay[s_pad.layout][i], text_alpha | 0xffffff00, ST_ABOVE_OVL);
    if (s_pad.cheats)
        ST_DrawString(40, y + 2, s_pad.layout ? "L2 + L3 god  R3 weapons  tri exit"
                                              : "L3 god  R3 weapons  triangle exit",
                      text_alpha | 0xffffff00, ST_ABOVE_OVL);

    ST_DrawString(-1, 210, "press circle to exit", text_alpha | 0xffffff00, ST_ABOVE_OVL);
}

/* ================================================================== */
/* Saves y configuracion (el "Controller Pak" es una carpeta del HDD)   */
/* ================================================================== */
#define SAVE_FILE     PS3_DATADIR "/doom64.sav"
#define SETTINGS_FILE PS3_DATADIR "/doom64.stg"

extern dirent_t FileState[200];
extern int32_t FilesUsed, Pak_Memory, Pak_Size;
extern uint8_t *Pak_Data;
int32_t ControllerPakStatus = 1;

static int file_size(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? (int)st.st_size : -1;
}

static int write_file(const char *path, const void *data, int size)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return -1;
    ok = (fwrite(data, 1, size, f) == (size_t)size);
    fclose(f);
    return ok ? 0 : -1;
}

static int read_file(const char *path, void *data, int size)
{
    FILE *f = fopen(path, "rb");
    int n;
    if (!f) return -1;
    n = (int)fread(data, 1, size, f);
    fclose(f);
    return n;
}

int I_CheckControllerPak(void)
{
    int n = 0, sz;
    mkdir(PS3_DATADIR, 0777);
    memset(FileState, 0, sizeof(dirent_t) * 200);
    Pak_Memory = 200;
    if ((sz = file_size(SAVE_FILE)) >= 0) {
        strcpy(FileState[n].name, "doom64");
        FileState[n].size = sz;
        Pak_Memory -= (sz + 511) / 512;
        n++;
    }
    if ((sz = file_size(SETTINGS_FILE)) >= 0) {
        strcpy(FileState[n].name, "doom64stg");
        FileState[n].size = sz;
        Pak_Memory -= (sz + 511) / 512;
        n++;
    }
    FilesUsed = n;
    ControllerPakStatus = 1;
    return 0;
}

int I_DeletePakFile(dirent_t *de)
{
    const char *path = !strcmp(de->name, "doom64stg") ? SETTINGS_FILE : SAVE_FILE;
    ps3_logf("[save] borrando %s", path);
    if (remove(path) != 0) return PFS_ERR_ID_FATAL;
    I_CheckControllerPak();
    return 0;
}

int I_SavePakSettings(doom64_settings_t *s)
{
    int r;
    mkdir(PS3_DATADIR, 0777);
    s->version = SETTINGS_SAVE_VERSION;
    r = write_file(SETTINGS_FILE, s, sizeof(*s));
    ps3_logf("[save] configuracion -> %s: %s", SETTINGS_FILE, r == 0 ? "ok" : "ERROR");
    return r == 0 ? 0 : PFS_ERR_ID_FATAL;
}

static int s_had_settings = 0;

int I_ReadPakSettings(doom64_settings_t *s)
{
    doom64_settings_t tmp;
    int n;
    memset(&tmp, 0, sizeof(tmp));
    n = read_file(SETTINGS_FILE, &tmp, sizeof(tmp));
    /* Version 3 (sin Gamma): se migra en vez de perder la configuracion. */
    if (n == (int)(sizeof(tmp) - sizeof(int)) && tmp.version == 3) {
        tmp.Gamma = PS3_GAMMA_OFF;
        tmp.version = SETTINGS_SAVE_VERSION;
        n = (int)sizeof(tmp);
        ps3_log("[save] configuracion v3 migrada a v4 (gamma = off)");
    }
    if (n != (int)sizeof(tmp) || tmp.version != SETTINGS_SAVE_VERSION) {
        ps3_logf("[save] sin configuracion guardada (%d bytes)", n);
        return PFS_ERR_ID_FATAL;
    }
    memcpy(s, &tmp, sizeof(tmp));
    s->runintroduction = false;
    if (s->Quality > q_medium) s->Quality = q_medium;   /* Ultra ya no existe */
    global_render_state.quality = s->Quality;
    global_render_state.fps_uncap = s->FpsUncap;
    s_had_settings = 1;
    ps3_logf("[save] configuracion leida: calidad %d, fps %s, sfx %d, musica %d",
             s->Quality, s->FpsUncap ? "libre" : "30", s->SfxVolume, s->MusVolume);
    return 0;
}

int I_ReadPakFile(void)
{
    Pak_Size = 512;
    if (!Pak_Data)
        Pak_Data = (uint8_t *)Z_Malloc(Pak_Size, PU_STATIC, NULL);
    memset(Pak_Data, 0, Pak_Size);
    if (read_file(SAVE_FILE, Pak_Data, Pak_Size) <= 0) {
        ps3_log("[save] no hay partidas guardadas");
        return PFS_ERR_ID_FATAL;
    }
    ControllerPakStatus = 1;
    ps3_log("[save] partidas leidas");
    return 0;
}

int I_SavePakFile(void)
{
    int r;
    if (!Pak_Data) return PFS_ERR_ID_FATAL;
    mkdir(PS3_DATADIR, 0777);
    r = write_file(SAVE_FILE, Pak_Data, 512);
    ps3_logf("[save] partidas -> %s: %s", SAVE_FILE, r == 0 ? "ok" : "ERROR");
    I_CheckControllerPak();
    return r == 0 ? 0 : PFS_ERR_ID_FATAL;
}

int I_CreatePakFile(void)
{
    Pak_Size = 512;
    if (!Pak_Data)
        Pak_Data = (uint8_t *)Z_Malloc(Pak_Size, PU_STATIC, NULL);
    memset(Pak_Data, 0, Pak_Size);
    ps3_log("[save] creando archivo de partidas");
    return I_SavePakFile();
}

/* ================================================================== */
/* Transiciones (i_main.c:1083-1336 del port de Dreamcast)              */
/* ================================================================== */
#define FB_TEX_W 512
#define FB_TEX_H 256
#define FB_TEX_SIZE ((FB_TEX_W) * (FB_TEX_H) * sizeof(uint16_t))

static pvr_vertex_t   wipeverts[12];
static pvr_poly_cxt_t wipecxt;
static pvr_poly_hdr_t wipehdr;

static void wipe_quad(pvr_vertex_t *v, float y0, float y1)
{
    const float u0 = 0.0f, u1 = 0.625f, v0 = 0.0f, v1 = 0.9375f;
    v[0].x = 0.0f;   v[0].y = y1; v[0].u = u0; v[0].v = v1;
    v[1].x = 0.0f;   v[1].y = y0; v[1].u = u0; v[1].v = v0;
    v[2].x = 640.0f; v[2].y = y1; v[2].u = u1; v[2].v = v1;
    v[3].x = 640.0f; v[3].y = y0; v[3].u = u1; v[3].v = v0;
}

/* PS3: la franja de arriba que deja libre la copia corrida (y de 0 a y0a)
 * se rellena de negro. En el Dreamcast esa franja quedaba quieta, con la
 * imagen original, pero la tapaba el overscan de la tele; aca se ve
 * entera, y ademas seguia "alimentando" la copia corrida con los colores
 * de arriba (rayas que no se apagaban). El negro sale de la misma captura:
 * las filas 240..255 de la textura quedan en 0. */
static void wipe_topfill(pvr_vertex_t *v, float y1)
{
    const float u0 = 0.0f, u1 = 0.625f, vt = 250.5f / 256.0f;
    v[0].x = 0.0f;   v[0].y = y1;   v[0].u = u0; v[0].v = vt;
    v[1].x = 0.0f;   v[1].y = 0.0f; v[1].u = u0; v[1].v = vt;
    v[2].x = 640.0f; v[2].y = y1;   v[2].u = u1; v[2].v = vt;
    v[3].x = 640.0f; v[3].y = 0.0f; v[3].u = u1; v[3].v = vt;
}

static void wipe_frame(int nverts)
{
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_list_finish();
    pvr_list_prim(PVR_LIST_TR_POLY, &wipehdr, sizeof(pvr_poly_hdr_t));
    pvr_list_prim(PVR_LIST_TR_POLY, wipeverts, nverts * sizeof(pvr_vertex_t));
    pvr_scene_finish();
}

void I_WIPE_FadeOutScreen(void);

void I_WIPE_MeltScreen(void)
{
    pvr_ptr_t pvrfb;
    uint16_t *fb;
    float y0a = 8.0f, y1a = 488.0f;
    int i, vn;

    ps3_log("[wipe] melt");
    fb = (uint16_t *)Z_Malloc(FB_TEX_SIZE, PU_STATIC, NULL);
    P_FlushAllCached();
    pvrfb = pvr_mem_malloc(FB_TEX_SIZE);
    if (!pvrfb) I_Error("PVR OOM for melt fb");
    memset(fb, 0, FB_TEX_SIZE);
    PS3_Plat_Capture565(fb);
    pvr_txr_load(fb, pvrfb, FB_TEX_SIZE);
    PS3_PVR_SetRawTexture(pvrfb);    /* captura de pantalla: ya tiene la gamma */

    pvr_poly_cxt_txr(&wipecxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     FB_TEX_W, FB_TEX_H, pvrfb, PVR_FILTER_NONE);
    wipecxt.gen.specular = PVR_SPECULAR_ENABLE;
    pvr_poly_compile(&wipehdr, &wipecxt);
    pvr_set_bg_color(0, 0, 0);
    pvr_fog_table_color(0.0f, 0.0f, 0.0f, 0.0f);
    pvr_fog_table_custom(empty_table);

    for (vn = 0; vn < 4; vn++) {
        wipeverts[vn].flags = PVR_CMD_VERTEX;
        wipeverts[vn].z = 4.9f;
        wipeverts[vn].argb = 0xFFFFFFFF;
        wipeverts[vn].oargb = 0;
    }
    wipeverts[3].flags = PVR_CMD_VERTEX_EOL;
    for (vn = 4; vn < 8; vn++) {
        wipeverts[vn].flags = PVR_CMD_VERTEX;
        wipeverts[vn].z = 5.0f;
        wipeverts[vn].argb = 0x36ffffff;
        wipeverts[vn].oargb = 0x160f0000;
    }
    wipeverts[7].flags = PVR_CMD_VERTEX_EOL;
    for (vn = 8; vn < 12; vn++) {
        wipeverts[vn] = wipeverts[4];
        wipeverts[vn].flags = PVR_CMD_VERTEX;
    }
    wipeverts[11].flags = PVR_CMD_VERTEX_EOL;

    for (i = 0; i < 320; i += 4) {
        wipe_quad(&wipeverts[0], 0.0f, 480.0f);
        wipe_quad(&wipeverts[4], y0a, y1a);
        wipe_topfill(&wipeverts[8], y0a);
        wipeverts[3].flags = PVR_CMD_VERTEX_EOL;
        wipeverts[7].flags = PVR_CMD_VERTEX_EOL;
        wipeverts[11].flags = PVR_CMD_VERTEX_EOL;
        wipe_frame(12);
        wipe_frame(12);
        if (i < (158 * 2)) {
            PS3_Plat_Capture565(fb);
            pvr_txr_load(fb, pvrfb, FB_TEX_SIZE);
            y0a += 1.0f;
            y1a += 1.0f;
        }
        if (PS3_Plat_ExitRequested()) break;
    }

    PS3_PVR_SetRawTexture(NULL);
    pvr_mem_free(pvrfb);
    Z_Free(fb);
    I_WIPE_FadeOutScreen();
}

void I_WIPE_FadeOutScreen(void)
{
    pvr_ptr_t pvrfb;
    uint16_t *fb;
    int i, vn;

    ps3_log("[wipe] fade");
    fb = (uint16_t *)Z_Malloc(FB_TEX_SIZE, PU_STATIC, NULL);
    P_FlushAllCached();
    pvrfb = pvr_mem_malloc(FB_TEX_SIZE);
    if (!pvrfb) I_Error("PVR OOM for fade fb");
    memset(fb, 0, FB_TEX_SIZE);
    PS3_Plat_Capture565(fb);
    pvr_txr_load(fb, pvrfb, FB_TEX_SIZE);
    PS3_PVR_SetRawTexture(pvrfb);    /* captura de pantalla: ya tiene la gamma */

    pvr_poly_cxt_txr(&wipecxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     FB_TEX_W, FB_TEX_H, pvrfb, PVR_FILTER_NONE);
    wipecxt.blend.src = PVR_BLEND_ONE;
    wipecxt.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&wipehdr, &wipecxt);
    pvr_set_bg_color(0, 0, 0);

    for (vn = 0; vn < 4; vn++) {
        wipeverts[vn].flags = PVR_CMD_VERTEX;
        wipeverts[vn].z = 5.0f;
        wipeverts[vn].oargb = 0;
    }
    wipeverts[3].flags = PVR_CMD_VERTEX_EOL;
    wipe_quad(&wipeverts[0], 0.0f, 480.0f);

    for (i = 248; i >= 0; i -= 8) {
        uint32_t c = 0xff000000u | ((uint32_t)i << 16) | ((uint32_t)i << 8) | (uint32_t)i;
        for (vn = 0; vn < 4; vn++) wipeverts[vn].argb = c;
        wipe_frame(4);
        if (PS3_Plat_ExitRequested()) break;
    }

    PS3_PVR_SetRawTexture(NULL);
    pvr_mem_free(pvrfb);
    Z_Free(fb);
}

/* ================================================================== */
/* Arranque: el resto de D_DoomMain (d_main.c) despues de las init      */
/* ================================================================== */
void PS3_Sys_Run(void)
{
    int exit;

    ps3_log("[sys] flujo del motor: splash, titulo, demos, menus");
    padcfg_load();

    early_error = 0;
    gamevbls = 0;
    gametic = 0;
    ticsinframe = 0;
    ticon = 0;
    ticbuttons[0] = 0;
    oldticbuttons[0] = 0;

    global_render_state.fps_uncap = 1;
    D_SplashScreen();

    M_ResetSettings(&menu_settings);       /* lee doom64.stg si existe */
    if (!s_had_settings) {
        /* Primera vez: calidad baja (la probada en hardware). Se puede subir
         * en Opciones > Video. */
        menu_settings.Quality = 0;
        global_render_state.quality = 0;
        global_render_state.fps_uncap = menu_settings.FpsUncap;
    }
    ps3_logf("[sys] calidad %d, fps %s", global_render_state.quality,
             global_render_state.fps_uncap ? "libre" : "30");
    P_RefreshBrightness();

    while (true) {
        exit = D_TitleMap();
        if (exit != ga_exit) {
            exit = D_RunDemo("DEMO1LMP", sk_medium, 3);
            if (exit != ga_exit) {
                exit = D_RunDemo("DEMO2LMP", sk_medium, 9);
                if (exit != ga_exit) {
                    exit = D_RunDemo("DEMO3LMP", sk_medium, 17);
                    if (exit != ga_exit) {
                        if (run_hectic_demo) {
                            run_hectic_demo = false;
                            exit = D_RunDemo("DEMO4LMP", sk_medium, 32);
                        }
                        if (exit != ga_exit) {
                            exit = D_Credits();
                            if (exit != ga_exit)
                                continue;
                        }
                    }
                }
            }
        }
        do {
            exit = M_RunTitle();
            ps3_logf("[sys] M_RunTitle -> %d", exit);
        } while (exit != ga_timeout);
    }
}
