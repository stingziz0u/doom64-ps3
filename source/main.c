/* main.c -- Doom64Cell, etapa 1a.
 *
 * Este binario todavia no es Doom 64. Es el esqueleto que valida, en orden y
 * dejando rastro en el log, cada eslabon que historicamente se rompe sin dar
 * ningun error:
 *
 *   [0] el binario llega a main()          -> si no, falta -lrt -llv2 en el link
 *   [1] thread de trabajo
 *   [2] callback de sysutil registrado     -> sin esto, salir por el boton PS reinicia
 *   [3] filesystem escribible
 *   [4] datos del juego + ENDIANNESS       <- lo nuevo de la etapa 1a
 *   [5] ioPad responde
 *   [6] RSX arranca
 *   [7] loop: pantalla de color cambiante
 *   [8] shutdown limpio                    -> salir sin reiniciar la consola
 *
 * Si la pantalla queda negra, el log dice hasta donde se llego.
 *
 * Salir: START, o "Quit Game" desde el XMB.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>
#include <lv2/systime.h>

#include "ps3_log.h"
#include "ps3_game.h"
#include "ps3_video.h"
#include "ps3_rsx.h"

void PS3_PVR_SetTextures(int on);     /* compat/ps3_pvr.c */
#include "ps3_pad.h"
#include "ps3_data.h"
#include "ps3_engine.h"

/* El loader lee esta seccion del ELF para dimensionar el stack del thread
 * primario. Sin ella el arranque es impredecible. */
SYS_PROCESS_PARAM(1001, 0x100000);

static volatile int s_running = 1;

/* ------------------------------------------------------------------ */
/* sysutil                                                             */
/* ------------------------------------------------------------------ */

static void sysutil_callback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;

    switch (status) {
        case SYSUTIL_EXIT_GAME:
            ps3_log("[sysutil] SYSUTIL_EXIT_GAME -> saliendo");
            s_running = 0;
            break;

        case SYSUTIL_DRAW_BEGIN:
        case SYSUTIL_DRAW_END:
            /* 0x121 / 0x131: overlay del XMB abriendose y cerrandose. Ignorar. */
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Etapa 3: filesystem                                                 */
/* ------------------------------------------------------------------ */

static void probe_dir(const char *path)
{
    DIR *d;
    struct dirent *e;
    int count = 0;

    d = opendir(path);
    if (!d) {
        ps3_logf("[fs] %s -> NO se puede abrir", path);
        return;
    }

    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (count < 24) ps3_logf("[fs]     %s", e->d_name);
        count++;
    }
    closedir(d);

    ps3_logf("[fs] %s -> OK, %d entradas", path, count);
}

static void probe_filesystem(void)
{
    FILE *f;

    ps3_log("[3] filesystem");

    /* USRDIR: sabemos que es escribible (el log ya vive ahi). */
    probe_dir(PS3_USRDIR);

    /* /dev_hdd0/data/doom64: aca queremos que vivan los datos del wadtool,
     * porque asi sobreviven a reinstalar el PKG (vamos a reinstalar mucho). */
    if (mkdir(PS3_DATADIR, 0777) == 0)
        ps3_logf("[fs] %s -> creado", PS3_DATADIR);

    f = fopen(PS3_DATADIR "/.writetest", "w");
    if (f) {
        fputs("ok", f);
        fclose(f);
        remove(PS3_DATADIR "/.writetest");
        ps3_logf("[fs] %s -> ESCRIBIBLE", PS3_DATADIR);
        probe_dir(PS3_DATADIR);
    } else {
        ps3_logf("[fs] %s -> NO escribible (caemos a USRDIR)", PS3_DATADIR);
    }
}

/* ------------------------------------------------------------------ */
/* Plataforma para compat/ps3_system.c: el juego corre con el flujo del   */
/* motor (titulo, menus, niveles) y le pide al port estas cosas.          */
/* ------------------------------------------------------------------ */
void PS3_Sys_Run(void);               /* compat/ps3_system.c */
int  PS3_RomConv_Check(const char *dir); /* ps3_romconv.c */

void PS3_Plat_PollInput(ps3_input_t *in)
{
    sysUtilCheckCallback();           /* sin bombear la cola, el evento no llega */
    PS3_Pad_Frame();
    in->buttons = PS3_Pad_Buttons();
    in->lx = PS3_Pad_LeftX();
    in->ly = PS3_Pad_LeftY();
    in->rx = PS3_Pad_RightX();
    in->ry = PS3_Pad_RightY();
}

int  PS3_Plat_ExitRequested(void) { return !s_running; }
u64  PS3_Plat_TimeNs(void)        { return (u64)sysGetSystemTime() * 1000ull; }
void PS3_Plat_Rumble(int s, int b) { PS3_Pad_Rumble(s, b); }
int  PS3_Plat_Capture565(u16 *dst) { PS3_Video_Capture565(dst); return 0; }
void PS3_Plat_SetDeadzone(int dz) { PS3_Pad_SetDeadzone(dz); }

void PS3_Plat_Exit(void)
{
    ps3_log("[8] shutdown");
    PS3_Video_Shutdown();             /* rsxFinish: el RSX ya no lee nada */
    PS3_RSX_Shutdown();
    PS3_Pad_Shutdown();
    ps3_log("[8] shutdown completo");
    ps3_log_shutdown();
    exit(0);
}

/* ------------------------------------------------------------------ */
/* Aviso al arrancar                                                   */
/*                                                                      */
/* El equivalente al warn3.dt del port de Dreamcast, del que viene este. */
/* Se ve 5 segundos o hasta apretar un boton.                            */
/* ------------------------------------------------------------------ */
static void PS3_Notice(void)
{
    static const char *const lines[] = {
        "Doom64-PS3 (Doom 64 for PS3) is free, open-source software,",
        "based on doom64-dc (jnmartin84) and DOOM64-RE (Erick194).",
        "",
        "It includes no game data: it needs your own Doom 64 ROM.",
        "Redistribution of game data is prohibited.",
    };
    u64 t0 = sysGetSystemTime();
    ps3_input_t in;
    int armed = 0;

    ps3_log("[aviso] pantalla de aviso");
    while (s_running && sysGetSystemTime() - t0 < 5000000ull) {
        PS3_Video_TextScreen("DOOM 64", lines, sizeof(lines) / sizeof(lines[0]), -1);
        PS3_Plat_PollInput(&in);
        /* primero soltar todo (por si venia apretado desde el XMB) */
        if (!in.buttons) armed = 1;
        else if (armed) break;
        usleep(30000);
    }
}

/* ------------------------------------------------------------------ */
/* Thread de trabajo                                                   */
/* ------------------------------------------------------------------ */

static void game_thread(void *arg)
{
    (void)arg;

    ps3_log("[1] game_thread arrancado");

    ps3_log("[2] registrando callback de sysutil");
    sysUtilRegisterCallback(0, sysutil_callback, NULL);

    probe_filesystem();

    /* Video y pad van ANTES que los datos: si hay que convertir la ROM (primer
     * arranque) o faltan archivos, se avisa en pantalla. */
    ps3_log("[5] pad");
    if (PS3_Pad_Init() < 0)
        ps3_log("[5] WARNING: ioPadInit fallo, se sigue sin pad");

    ps3_log("[6] video");
    if (PS3_Video_Init() < 0) {
        ps3_log("[6] FATAL: no se pudo inicializar el RSX, abortando");
        goto done;
    }

    ps3_log("[6b] backend RSX (shaders + ring de vertices)");
    if (PS3_RSX_Init() < 0)
        ps3_log("[6b] WARNING: el backend RSX no arranco; se sigue con el color del sector");

    PS3_Notice();

    ps3_log("[3b] datos desde la ROM (primer arranque)");
    if (PS3_RomConv_Check(PS3_USRDIR) != 0)
        goto done;

    ps3_log("[4] datos del juego");
    PS3_Data_Probe(PS3_USRDIR);

    ps3_log("[4b] motor");
    PS3_Engine_Probe(PS3_USRDIR);

    /* Interruptor sin recompilar: un archivo NOTEX (vacio) en USRDIR apaga
     * las texturas y deja la geometria con colores planos. Sirve para
     * separar un problema de texturas de uno del pipeline base. */
    if (access(PS3_USRDIR "/NOTEX", F_OK) == 0) {
        PS3_PVR_SetTextures(0);
        ps3_log("[6b] archivo NOTEX presente: texturas APAGADAS");
    }

    ps3_log("[7] juego: titulo y menus del motor. Salir: boton PS -> Salir del juego");
    PS3_Sys_Run();                    /* no vuelve: sale por PS3_Plat_Exit */

done:
    ps3_log("[8] shutdown");
    { extern void PS3_Audio_Shutdown(void); PS3_Audio_Shutdown(); }
    PS3_Video_Shutdown();       /* rsxFinish: el RSX ya no lee nada */
    PS3_RSX_Shutdown();
    PS3_Pad_Shutdown();
    ps3_log("[8] shutdown completo");
    ps3_log_shutdown();

    sysThreadExit(0);
}

int main(int argc, char **argv)
{
    sys_ppu_thread_t tid;
    u64 exit_code;

    (void)argc;
    (void)argv;

    ps3_log_init();
    ps3_log("[0] main() alcanzado -- el link esta bien (-lrt -llv2 presentes)");

    /* El thread por defecto del EBOOT tiene un stack chico: el trabajo real
     * va en un thread propio. */
    if (sysThreadCreate(&tid, game_thread, NULL, 1000, 2 * 1024 * 1024,
                        THREAD_JOINABLE, "doom64") != 0) {
        ps3_log("[0] FATAL: sysThreadCreate fallo");
        ps3_log_shutdown();
        return 1;
    }

    sysThreadJoin(tid, &exit_code);
    return 0;
}
