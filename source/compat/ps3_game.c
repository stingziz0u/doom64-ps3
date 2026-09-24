/* ps3_game.c -- P_SetupLevel paso a paso, y la logica del juego corriendo
 * sin dibujar (etapas 2a + 2b).
 *
 * Por que este archivo incluye los headers del motor y ps3_engine.c no:
 * aca se tocan structs enteras (player_t, mobj_t, thinker_t), y declararlas
 * a mano seria copiar medio doomdef.h. ps3_engine.c solo mira contadores.
 *
 * ---------------------------------------------------------------------------
 * LA LECCION DE LA ETAPA 2a
 *
 * La version anterior de load_map() llamaba solo a los P_Load* de
 * P_SetupLevel, sin el PREAMBULO. Ese preambulo no es opcional:
 *
 *     thinkercap.prev = thinkercap.next = &thinkercap;
 *     mobjhead.next   = mobjhead.prev   = &mobjhead;
 *
 * Son listas circulares con centinela: vacias, apuntan a si mismas. Sin
 * inicializar quedan en NULL, y el primer P_SpawnMobj hace
 *     mobjhead.prev->next = mobj;     -> escribe en NULL->next
 * Crash duro sin I_Error: el log corto en "-> P_LoadThings". Hasta ahi no
 * se notaba porque ningun P_Load* anterior spawnea nada.
 *
 * Por eso ahora se replica P_SetupLevel COMPLETO, en el mismo orden, y con
 * el mismo log-antes-de-ejecutar de siempre.
 * ------------------------------------------------------------------------- */

#include "doomdef.h"
#include "p_local.h"
#include "r_local.h"
#include "../ps3_log.h"
#include "../ps3_game.h"

/* --- del motor: sin prototipo en ningun header --- */
void P_LoadMacros(void);
void P_LoadBlockMap(void);
void P_LoadVertexes(void);
void P_LoadSectors(void);
void P_LoadSideDefs(void);
void P_LoadLineDefs(void);
void P_LoadSubSectors(void);
void P_LoadNodes(void);
void P_LoadSegs(void);
void P_LoadLeafs(void);
void P_LoadReject(void);
void P_LoadLights(void);
void P_GroupLines(void);
void P_LoadThings(void);
void P_SpawnSpecials(void);
void W_OpenMapWad(int mapnum);
void W_FreeMapLump(void);

extern int add_lightning;
extern int spawncount;
extern mobj_t *rp1_rk, *rp1_bk, *rp1_yk;
extern uint32_t NextFrameIdx;
extern pvr_dr_state_t dr_state;       /* d_main.c */
void P_Drawer(void);
extern int last_Ltrig, last_Rtrig;    /* ps3_platform_stub.c; P_BuildMove los usa para el strafe analogico */

void M_ResetSettings(doom64_settings_t *s);
void P_RefreshBrightness(void);
void G_InitNew(skill_t skill, int map, gametype_t gametype);

/* Las piezas de P_Ticker (p_tick.c:217), para la sonda del primer tic. */
void P_CheckCheats(void);
void P_RunThinkers(void);
void P_CheckSights(void);
void P_RunMobjBase(void);
void P_UpdateSpecials(void);
void P_RunMacros(void);
void ST_Ticker(void);
void AM_Control(player_t *player);
void P_PlayerThink(player_t *player);

#define STEP(fn)  do { ps3_log("[game]   -> " #fn); fn; } while (0)

/* ------------------------------------------------------------------ */
/* Contadores de las listas del motor                                   */
/* ------------------------------------------------------------------ */

static int count_mobjs(void)
{
    mobj_t *mo;
    int n = 0;
    for (mo = mobjhead.next; mo && mo != &mobjhead; mo = mo->next)
        if (++n > 65536) return -1;          /* lista rota: no colgarse */
    return n;
}

static int count_thinkers(void)
{
    thinker_t *th;
    int n = 0;
    for (th = thinkercap.next; th && th != &thinkercap; th = th->next)
        if (++n > 65536) return -1;
    return n;
}

/* Los que usa ps3_engine.c (no incluye los headers del motor). */
int PS3_CountMobjs(void)     { return count_mobjs(); }
int PS3_GetSpawnCount(void)  { return spawncount; }

/* ------------------------------------------------------------------ */
/* P_SetupLevel (p_setup.c:993), replicado paso por paso                 */
/* ------------------------------------------------------------------ */

static int s_ready = 0;

static void load_level(int map);

void PS3_Game_NewGame(int map)
{
    ps3_logf("[game] --- juego nuevo en el mapa %d ---", map);

    /* ---- lo que corre ANTES de P_SetupLevel en el juego real ----
     *
     * Camino real: D_DoomMain -> M_ResetSettings + P_RefreshBrightness ->
     * menu -> G_InitNew -> G_RunGame -> G_DoLoadLevel -> P_SetupLevel.
     *
     * Saltearse G_InitNew fue el crash del build anterior: es el UNICO lugar
     * que hace  BT_DATA[0] = ActualConfiguration  (el mapeo de botones), y
     * P_PlayerThink hace  cbutton = BT_DATA[0]; cbutton->BT_WEAPONBACKWARD
     * en el primer tic -> lectura de NULL, crash mudo. Mismo patron que la
     * vez de mobjhead: no replicar a mano las inicializaciones del motor,
     * llamar a las funciones reales. */
    STEP(M_ResetSettings(&menu_settings));
    STEP(P_RefreshBrightness());
    STEP(G_InitNew(sk_medium, map, gt_single));
    ps3_logf("[game]      BT_DATA[0]=%p  gamemap=%d  gameskill=%d",
             (void *)BT_DATA[0], gamemap, (int)gameskill);
    if (!BT_DATA[0])
        I_Error("BT_DATA[0] sigue en NULL despues de G_InitNew");

    load_level(map);
}

/* G_DoLoadLevel + P_SetupLevel. Se usa para el arranque, al morir (mismo
 * mapa, el jugador renace) y al terminar un nivel (mapa siguiente, el
 * jugador conserva armas y vida, igual que G_RunGame). */
static void load_level(int map)
{
    int memory;

    s_ready = 0;
    gamemap = map;
    ps3_logf("[game] --- P_SetupLevel(%d) ---", map);

    /* G_DoLoadLevel */
    if (players[0].playerstate == PST_DEAD)
        players[0].playerstate = PST_REBORN;

    /* ---- preambulo: lo que faltaba ---- */
    add_lightning = 0;
    Z_FreeTags(mainzone, ~PU_STATIC);
    Z_CheckZone(mainzone);
    M_ClearRandom();

    rp1_rk = rp1_bk = rp1_yk = NULL;
    totalkills = totalitems = totalsecret = 0;

    thinkercap.prev = thinkercap.next = &thinkercap;
    mobjhead.next   = mobjhead.prev   = &mobjhead;
    spawncount = 0;
    ps3_log("[game]   listas inicializadas (thinkercap, mobjhead)");

    /* ---- carga ---- */
    STEP(W_OpenMapWad(map));
    STEP(P_LoadMacros());
    STEP(P_LoadBlockMap());
    STEP(P_LoadVertexes());
    STEP(P_LoadSectors());
    STEP(P_LoadSideDefs());
    STEP(P_LoadLineDefs());
    STEP(P_LoadSubSectors());
    STEP(P_LoadNodes());
    STEP(P_LoadSegs());
    STEP(P_LoadLeafs());
    STEP(P_LoadReject());
    STEP(P_LoadLights());
    STEP(P_GroupLines());
    STEP(P_LoadThings());
    ps3_logf("[game]      mobjs=%d  diferidos=%d  kills=%d items=%d secretos=%d",
             count_mobjs(), spawncount, totalkills, totalitems, totalsecret);
    STEP(W_FreeMapLump());

    /* ---- mundo ---- */
    STEP(P_Init());            /* cachea las 503 texturas: DecodeD64 a full */
    STEP(P_SpawnSpecials());
    STEP(R_SetupSky());

    STEP(Z_SetAllocBase(mainzone));
    STEP(Z_CheckZone(mainzone));
    STEP(Z_Defragment(mainzone));

    memory = Z_FreeMemory(mainzone);
    ps3_logf("[game]   heap libre: %d KB", memory / 1024);
    if (memory < 0x10000)
        I_Error("not enough free memory %d", memory);

    STEP(P_SpawnPlayer());

    {
        player_t *p = &players[0];
        if (!p->mo) {
            ps3_log("[game] *** P_SpawnPlayer no dejo players[0].mo ***");
            return;
        }
        ps3_logf("[game]   jugador en (%d, %d) z=%d  piso=%d  vida=%d  angulo=%u",
                 p->mo->x >> FRACBITS, p->mo->y >> FRACBITS, p->mo->z >> FRACBITS,
                 p->mo->floorz >> FRACBITS, p->health,
                 (unsigned)(p->mo->angle >> 24) * 360 / 256);
        /* El log de la etapa [4] dice: player start = (152, -1344), angulo 180 */
        if ((p->mo->x >> FRACBITS) == 152 && (p->mo->y >> FRACBITS) == -1344)
            ps3_log("[game]     OK: el jugador esta en el player start del MAP01");
        if (p->mo->z == p->mo->floorz)
            ps3_log("[game]     OK: parado en el piso de su sector");
        if (p->health == 100)
            ps3_log("[game]     OK: vida 100 (G_PlayerReborn corrio)");
    }

    ps3_logf("[game] --- nivel listo: %d mobjs, %d thinkers ---",
             count_mobjs(), count_thinkers());
    s_ready = 1;
}

int PS3_Game_Ready(void) { return s_ready; }

/* ------------------------------------------------------------------ */
/* Tics sin dibujar                                                      */
/*                                                                       */
/* Es MiniLoop (d_main.c:254) reducido a lo que toca la logica: avanzar  */
/* gametic, dejar los botones, llamar a P_Ticker, y marcar gamevbls. La  */
/* parte de tiempos reales y la de dibujo no hacen falta: aca cada vuelta */
/* es exactamente un tic (1/30 s del juego).                             */
/* ------------------------------------------------------------------ */

static int s_tic = 0;      /* ultimo gametic corrido */

int PS3_Game_RunTics(int ntics)
{
    int t, ga = ga_nothing;
    player_t *p = &players[0];

    ps3_logf("[game] --- corriendo %d tics sin dibujar ---", ntics);

    gameaction = ga_nothing;
    gametic = gamevbls = 0;
    f_gametic = f_gamevbls = 0.0f;
    ticbuttons[0] = oldticbuttons[0] = 0;

    /* Vblanks por tic. MiniLoop lo recalcula en cada frame; P_PlayerThink y
     * P_BuildMove escalan movimiento y timers de powerups con esto. En 0 el
     * jugador no se moveria nunca. Aca cada vuelta es exactamente un tic de
     * 30 Hz = 2 vblanks de 60 Hz (ticon += 2 -> ticsinframe += 1). */
    vblsinframe[0]   = 2;
    f_vblsinframe[0] = 2.0f;

    STEP(P_Start());   /* thinker de fade-in, lineas con tag 999, musica (stub) */

    for (t = 1; t <= ntics; t++) {
        oldticbuttons[0] = ticbuttons[0];
        ticbuttons[0] = 0;                 /* nadie toca el mando todavia */

        gametic   = t;
        f_gametic = (float)t;              /* f_gamevbls < f_gametic -> corre */

        if (t == 1) {
            /* SONDA: el primer tic se corre pieza por pieza, en el mismo
             * orden que P_Ticker y logueando antes de cada una. Si algo mas
             * lee un puntero sin inicializar, la ultima linea dice cual.
             * (Se saltea P_RecordOldPositions, que es static y en el tic 1
             * solo copia x,y,z -> old_x,y,z; y la logica de pausa, que con
             * gamepaused=0 no hace nada.) Del tic 2 en adelante, P_Ticker
             * real. */
            gameaction = ga_nothing;
            ps3_log("[tic   1] sonda: P_Ticker pieza por pieza");
            STEP(P_CheckCheats());
            STEP(P_RunThinkers());
            STEP(P_CheckSights());
            STEP(P_RunMobjBase());
            STEP(P_UpdateSpecials());
            STEP(P_RunMacros());
            STEP(ST_Ticker());
            STEP(AM_Control(p));
            STEP(P_PlayerThink(p));
            ga = gameaction;
            ps3_log("[tic   1] sonda completa: todas las piezas de P_Ticker corren");
        } else {
            ga = P_Ticker();
        }

        gamevbls   = gametic;
        f_gamevbls = f_gametic;
        NextFrameIdx++;

        /* Chequeo del zone en CADA tic: si algo pisa memoria, I_Error con
         * mensaje en el tic exacto, en vez de un crash mudo mucho despues. */
        Z_CheckZone(mainzone);

        if (t == 1 || (t % 30) == 0) {
            ps3_logf("[tic %3d] jugador (%d,%d,%d) vida=%d  mobjs=%d thinkers=%d  heap=%dKB",
                     t,
                     p->mo ? p->mo->x >> FRACBITS : 0,
                     p->mo ? p->mo->y >> FRACBITS : 0,
                     p->mo ? p->mo->z >> FRACBITS : 0,
                     p->health, count_mobjs(), count_thinkers(),
                     Z_FreeMemory(mainzone) / 1024);
        }

        if (ga != ga_nothing) {
            ps3_logf("[game] P_Ticker devolvio gameaction=%d en el tic %d", ga, t);
            break;
        }
    }

    /* Sin P_Stop: el nivel queda cargado y el loop en vivo sigue desde aca
     * (PS3_Game_Frame). */
    s_tic = gametic;

    if (t > ntics)
        ps3_logf("[game] OK: %d tics (%d segundos de juego) sin romper nada",
                 ntics, ntics / 30);
    return ga;
}

/* ==================================================================
 * ETAPA 2c: el juego en vivo con el DualShock 3
 *
 * Todavia no hay renderer, asi que la pantalla muestra el COLOR DE LUZ DEL
 * SECTOR donde esta parado el jugador (el mismo que el motor usa para
 * iluminar los things). Al caminar de una sala a otra, cambia el color.
 * Recibir dano tine de rojo y agarrar items de dorado: son los mismos
 * damagecount/bonuscount que el juego usa para su flash de pantalla.
 * ================================================================== */

/* ------------------------------------------------------------------ */
/* Mando: DualShock 3 -> formato de N64 que espera el motor              */
/*                                                                      */
/* El motor lee ticbuttons[0] con el layout del mando de N64:           */
/*   bits 31..16  botones digitales (PAD_*)                             */
/*   bits 15..8   stick X (int8) -> giro, o strafe si BT_STRAFE          */
/*   bits  7..0   stick Y (int8) -> adelante / atras                     */
/* y el strafe analogico sale de PAD_L/R_TRIG + last_Ltrig/last_Rtrig    */
/* (0..255), igual que hace i_main.c con el segundo stick del Dreamcast. */
/*                                                                      */
/* Los PAD_* de aca son los del MOTOR. El mapeo boton->accion lo resuelve */
/* el propio motor con BT_DATA[0] = ActualConfiguration (m_main.c:423):   */
/*   PAD_UP/DOWN adelante/atras   PAD_LEFT/RIGHT girar                    */
/*   PAD_Z_TRIG disparar          PAD_RIGHT_C usar     PAD_UP_C automapa   */
/*   PAD_LEFT_C correr/caminar    PAD_L/R_TRIG strafe                      */
/*   PAD_A arma anterior          PAD_B arma siguiente                     */
/* ------------------------------------------------------------------ */

static int clamp127(int v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;   /* -128 da vuelta el signo al negarlo */
    return v;
}

static int build_buttons(const ps3_input_t *in)
{
    unsigned short d = in->buttons;
    int b = 0;

    /* Cruceta: igual que la del N64. */
    if (d & DS3_UP)    b |= PAD_UP;
    if (d & DS3_DOWN)  b |= PAD_DOWN;
    if (d & DS3_LEFT)  b |= PAD_LEFT;
    if (d & DS3_RIGHT) b |= PAD_RIGHT;

    if (d & (DS3_R2 | DS3_SQUARE)) b |= PAD_Z_TRIG;    /* disparar */
    if (d & DS3_CROSS)             b |= PAD_RIGHT_C;   /* usar / abrir */
    if (d & DS3_L1)                b |= PAD_A;         /* arma anterior */
    if (d & DS3_R1)                b |= PAD_B;         /* arma siguiente */
    if (d & DS3_SELECT)            b |= PAD_UP_C;      /* automapa */
    if (d & DS3_L2)                b |= PAD_LEFT_C;    /* caminar (autorun ON) */

    /* Stick izquierdo X: strafe con intensidad proporcional. */
    last_Ltrig = last_Rtrig = 0;
    if (in->lx < 0) {
        b |= PAD_L_TRIG;
        last_Ltrig = (-in->lx * 2 > 255) ? 255 : -in->lx * 2;
    } else if (in->lx > 0) {
        b |= PAD_R_TRIG;
        last_Rtrig = (in->lx * 2 > 255) ? 255 : in->lx * 2;
    }

    /* Stick izquierdo Y: adelante/atras. En el DS3 arriba es negativo, en el
     * motor adelante es positivo. */
    b |= clamp127(-in->ly) & 0xff;

    /* Stick derecho X: giro. Positivo = derecha en los dos. */
    b |= (clamp127(in->rx) & 0xff) << 8;

    return b;
}

/* ------------------------------------------------------------------ */
/* Log de lo que pasa en el juego (lo unico visible sin renderer)        */
/* ------------------------------------------------------------------ */

static const char *weapon_name(int w)
{
    /* Mismo orden que weapontype_t (doomdef.h:924). */
    static const char *names[] = {
        "motosierra", "punos", "pistola", "escopeta", "super escopeta",
        "chaingun", "lanzacohetes", "plasma", "BFG", "laser"
    };
    if (w >= 0 && w < (int)(sizeof(names) / sizeof(names[0])))
        return names[w];
    return "?";
}

static const char *ga_name(int ga)
{
    switch (ga) {
    case ga_died:       return "murio";
    case ga_completed:  return "nivel completado";
    case ga_secretexit: return "salida secreta";
    case ga_warped:     return "warp";
    case ga_restart:    return "restart";
    case ga_exit:       return "exit";
    default:            return "?";
    }
}

static int   s_frame      = 0;
static int   s_last_btn   = 0;
static int   s_last_x     = 0x7fffffff, s_last_y = 0x7fffffff;
static int   s_last_sec   = -1;
static int   s_last_weap  = -1;
static int   s_last_hp    = -1;
static int   s_last_kills = -1;
static char *s_last_msg   = NULL;

static void log_changes(const ps3_input_t *in, int engine_buttons)
{
    player_t *p = &players[0];
    mobj_t   *mo = p->mo;
    int x, y, sec;

    /* Mando: solo cuando cambian los botones DIGITALES (los sticks cambian
     * todo el tiempo y taparian el log). */
    if ((engine_buttons & 0xffff0000) != (s_last_btn & 0xffff0000)) {
        ps3_logf("[input] ds3=0x%04x -> motor=0x%08x  (lx=%d ly=%d rx=%d)",
                 (unsigned)in->buttons, (unsigned)engine_buttons,
                 in->lx, in->ly, in->rx);
    }
    s_last_btn = engine_buttons;

    if (!mo) return;

    x = mo->x >> FRACBITS;
    y = mo->y >> FRACBITS;
    sec = (mo->subsector && mo->subsector->sector)
        ? (int)(mo->subsector->sector - sectors) : -1;

    /* Posicion: cada medio segundo, si se movio. */
    if ((s_tic % 15) == 0 && (x != s_last_x || y != s_last_y)) {
        ps3_logf("[mov] tic %d  (%d, %d) z=%d  angulo=%u  sector=%d",
                 s_tic, x, y, mo->z >> FRACBITS,
                 (unsigned)(mo->angle >> 24) * 360 / 256, sec);
        s_last_x = x;
        s_last_y = y;
    }

    if (sec != s_last_sec) {
        if (s_last_sec >= 0 && sec >= 0) {
            sector_t *s = &sectors[sec];
            ps3_logf("[mov] entro al sector %d  (piso %d, techo %d, luz 0x%08x)",
                     sec, s->floorheight >> FRACBITS, s->ceilingheight >> FRACBITS,
                     (unsigned)lights[s->colors[2]].rgba);
        }
        s_last_sec = sec;
    }

    if ((int)p->readyweapon != s_last_weap) {
        ps3_logf("[game] arma: %s", weapon_name(p->readyweapon));
        s_last_weap = p->readyweapon;
    }

    if (p->health != s_last_hp) {
        if (s_last_hp >= 0)
            ps3_logf("[game] vida %d -> %d", s_last_hp, p->health);
        s_last_hp = p->health;
    }

    if (p->killcount != s_last_kills) {
        if (s_last_kills >= 0)
            ps3_logf("[game] kills: %d / %d", p->killcount, totalkills);
        s_last_kills = p->killcount;
    }

    /* Los mensajes del HUD ("You got the shotgun!", "You need a blue key"...)
     * son el mejor indicio de que las interacciones funcionan. */
    if (p->message && p->message != s_last_msg)
        ps3_logf("[hud] %s", p->message);
    s_last_msg = p->message;
}

/* ------------------------------------------------------------------ */
/* Muerte y fin de nivel: lo que hace G_RunGame entre MiniLoops          */
/* ------------------------------------------------------------------ */

static void start_level(void)
{
    /* Lo que MiniLoop pone en cero al arrancar cada nivel. */
    gameaction = ga_nothing;
    gametic = gamevbls = 0;
    f_gametic = f_gamevbls = 0.0f;
    ticbuttons[0] = oldticbuttons[0] = 0;
    s_tic = 0;
    s_last_x = s_last_y = 0x7fffffff;
    s_last_sec = -1;
    P_Start();
}

static void handle_gameaction(int ga)
{
    int map = gamemap;

    ps3_logf("[game] === %s (gameaction=%d) en el tic %d ===", ga_name(ga), ga, s_tic);

    P_Stop(ga);

    switch (ga) {
    case ga_died:
    case ga_restart:
        /* Mismo mapa. P_Ticker ya dejo playerstate en PST_REBORN, asi que
         * P_SpawnPlayer llama a G_PlayerReborn: vida 100, pistola. */
        break;

    case ga_completed:
    case ga_secretexit:
        map = nextmap;
        ps3_logf("[game] siguiente mapa: %d", map);
        break;

    default:
        break;
    }

    if (map < 1 || map > 50) {
        ps3_logf("[game] mapa %d fuera de rango: juego nuevo en el 1", map);
        PS3_Game_NewGame(1);
    } else {
        load_level(map);
    }

    start_level();
}

/* ------------------------------------------------------------------ */

void PS3_Game_Frame(const ps3_input_t *in)
{
    int ga, eb;

    if (!s_ready)
        return;

    /* El video va a 60 Hz y el juego a 30: un tic cada dos frames. */
    if ((++s_frame & 1) != 0)
        return;

    eb = build_buttons(in);

    oldticbuttons[0] = ticbuttons[0];
    ticbuttons[0]    = eb;

    s_tic++;
    gametic   = s_tic;
    f_gametic = (float)s_tic;
    vblsinframe[0]   = 2;
    f_vblsinframe[0] = 2.0f;

    ga = P_Ticker();

    gamevbls   = gametic;
    f_gamevbls = f_gametic;
    /* NextFrameIdx lo avanza I_ClearFrame en cada frame dibujado. */

    Z_CheckZone(mainzone);

    log_changes(in, eb);

    if (ga != ga_nothing)
        handle_gameaction(ga);
}

/* ------------------------------------------------------------------ */

static unsigned int mix(unsigned int c, unsigned int target, int amount)
{
    /* amount 0..255 */
    int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    int tr = (target >> 16) & 0xff, tg = (target >> 8) & 0xff, tb = target & 0xff;
    r += ((tr - r) * amount) / 255;
    g += ((tg - g) * amount) / 255;
    b += ((tb - b) * amount) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

unsigned int PS3_Game_ViewColor(void)
{
    player_t *p = &players[0];
    sector_t *sec;
    unsigned int rgba, c;
    int r, g, b, m;

    if (!s_ready || !p->mo || !p->mo->subsector || !lights)
        return 0xFF101010u;

    sec = p->mo->subsector->sector;
    if (sec->colors[2] < 0 || sec->colors[2] >= numlights)
        return 0xFF101010u;

    rgba = (unsigned int)lights[sec->colors[2]].rgba;    /* 0xRRGGBBAA */
    r = (rgba >> 24) & 0xff;
    g = (rgba >> 16) & 0xff;
    b = (rgba >>  8) & 0xff;

    /* Normalizar el brillo: los colores de Doom 64 son oscuros y lo que
     * importa aca es el TONO, que es lo que cambia de sala en sala. */
    m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (m > 0 && m < 200) {
        r = r * 200 / m;
        g = g * 200 / m;
        b = b * 200 / m;
    }
    c = 0xFF000000u | (r << 16) | (g << 8) | b;

    if (p->damagecount > 0)
        c = mix(c, 0xFFFF0000u, p->damagecount * 8 > 220 ? 220 : p->damagecount * 8);
    else if (p->bonuscount > 0)
        c = mix(c, 0xFFFFD040u, p->bonuscount * 16 > 200 ? 200 : p->bonuscount * 16);

    return c;
}

void PS3_Game_Shutdown(void)
{
    if (!s_ready)
        return;
    ps3_log("[game] P_Stop (saliendo)");
    P_Stop(ga_exit);
    s_ready = 0;
}

/* ------------------------------------------------------------------ */
/* Dibujo: lo que hace MiniLoop (d_main.c:388-420) alrededor del drawer  */
/* ------------------------------------------------------------------ */

void PS3_Game_Draw(void)
{
    static int frames = 0;

    if (!s_ready)
        return;

    frames++;
    if (frames == 1)
        ps3_log("[draw] primer frame: P_Drawer (render del motor)...");

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_init(&dr_state);

    P_Drawer();

    pvr_list_finish();
    pvr_scene_finish();          /* el emulador del TA dibuja en el RSX */

    if (frames == 1)
        ps3_log("[draw] primer frame dibujado");
}
