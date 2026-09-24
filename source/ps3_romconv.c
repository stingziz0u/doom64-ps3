/* ps3_romconv.c -- primer arranque: datos del juego desde la ROM de N64.
 *
 * El port de Dreamcast (y hasta ahora este) necesitaba correr el wadtool en
 * una PC para sacar de la ROM pow2.wad, alt.wad, tex/non_enemy.tex y los
 * mapas. Ahora el wadtool corre aca (source/romconv), una sola vez:
 *
 *   USRDIR/<cualquier nombre>.z64 | .v64 | .n64   la ROM de Doom 64 (USA)
 *   USRDIR/DOOM64.WAD (opcional)                  el del remaster de
 *                                                 Nightdive: Lost Levels
 *
 * Si los datos ya estan (convertidos antes, o subidos a mano desde la PC)
 * no se hace nada. Si una conversion se corta a la mitad (se apago la
 * consola), queda el archivo romconv.run y en el proximo arranque se rehace.
 *
 * De la ROM sale todo lo que el motor necesita para arrancar (incluidos
 * symbols.raw y tex/wepn_decs.raw, que doom64-dc traia hechos). El PKG no
 * trae datos del juego. Opcional, del repo de doom64-dc: Knee Deep in the
 * Dead (maps/map41-49.wad + doom1mn.lmp + su musica).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <ppu-types.h>
#include <sysutil/sysutil.h>
#include <lv2/systime.h>

#include "ps3_log.h"
#include "ps3_video.h"

int PS3_RomConv_Rom(const char *romfn, const char *outdir);
int PS3_RomConv_Lost(const char *wadfn, const char *outdir);
int PS3_Plat_ExitRequested(void);

#define ROM_MARK_RUN  "romconv.run"   /* existe mientras se convierte */

/* Rutas armadas con nombres de readdir (d_name llega a 1024 en PSL1GHT) */
#define RC_PATH 1536

/* ------------------------------------------------------------------ */
/* Log y progreso (los llama el wadtool)                                */
/* ------------------------------------------------------------------ */
void rc_vlog(const char *fmt, va_list ap)
{
    char buf[512];
    size_t n;
    vsnprintf(buf, sizeof(buf), fmt, ap);
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (n) ps3_logf("[rom] %s", buf);
}

void rc_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rc_vlog(fmt, ap);
    va_end(ap);
}

/* Peso de cada paso en la barra (medido: la mitad se va en los sprites). */
static const int s_step_start[10] = { 0, 0, 1, 28, 33, 77, 78, 96, 100, 100 };
static int s_step = 0;
static const char *s_what = "";
static int s_mode_lost = 0;
static u64 s_last_draw = 0;
static int s_last_pct = -1;

static u64 now_us(void) { return sysGetSystemTime(); }

static void draw_progress(int pct)
{
    char l1[96], l2[96];
    const char *lines[5];
    if (s_mode_lost) {
        snprintf(l1, sizeof(l1), "Converting the Lost Levels from the remaster");
        snprintf(l2, sizeof(l2), "%s", s_what);
    } else {
        snprintf(l1, sizeof(l1), "First boot: converting the N64 ROM");
        snprintf(l2, sizeof(l2), "Step %d of 7: %s", s_step, s_what);
    }
    lines[0] = l1;
    lines[1] = "This is done only once. It takes a minute or two.";
    lines[2] = "";
    lines[3] = l2;
    lines[4] = "Do not turn off the console.";
    PS3_Video_TextScreen("DOOM 64", lines, 5, pct);
    sysUtilCheckCallback();
    s_last_draw = now_us();
    s_last_pct = pct;
}

void rc_progress(int step, const char *what)
{
    s_step = step;
    s_what = what;
    ps3_logf("[rom] paso %d: %s", step, what);
    draw_progress(s_mode_lost ? -1 : s_step_start[step]);
}

void rc_subprogress(int done, int total)
{
    int a, b, pct;
    if (s_mode_lost || total <= 0 || s_step < 1 || s_step > 7) return;
    a = s_step_start[s_step];
    b = s_step_start[s_step + 1];
    pct = a + (b - a) * done / total;
    /* redibujar cuesta: como mucho 4 veces por segundo */
    if (pct != s_last_pct && now_us() - s_last_draw > 250000)
        draw_progress(pct);
}

/* ------------------------------------------------------------------ */
/* Archivos                                                             */
/* ------------------------------------------------------------------ */
static int exists(const char *dir, const char *name)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return stat(p, &st) == 0 && st.st_size > 0;
}

static int rom_data_complete(const char *dir)
{
    char n[32];
    int i;
    if (exists(dir, ROM_MARK_RUN)) {
        ps3_log("[rom] quedo una conversion a medias: se rehace");
        return 0;
    }
    if (!exists(dir, "pow2.wad") || !exists(dir, "alt.wad") || !exists(dir, "tex/non_enemy.tex") ||
        !exists(dir, "symbols.raw") || !exists(dir, "tex/wepn_decs.raw"))
        return 0;
    for (i = 1; i <= 33; i++) {
        snprintf(n, sizeof(n), "maps/map%02d.wad", i);
        if (!exists(dir, n)) return 0;
    }
    return 1;
}

static int lost_complete(const char *dir)
{
    char n[32];
    int i;
    for (i = 34; i <= 40; i++) {
        snprintf(n, sizeof(n), "maps/map%d.wad", i);
        if (!exists(dir, n)) return 0;
    }
    return 1;
}

static int has_ext(const char *name, const char *ext)
{
    size_t a = strlen(name), b = strlen(ext);
    return a > b && !strcasecmp(name + a - b, ext);
}

/* La ROM: primero por extension; si no, cualquier archivo de 8 MB cuya
 * cabecera sea de N64 (en cualquiera de los tres ordenes de bytes). */
static int find_rom(const char *dir, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (has_ext(e->d_name, ".z64") || has_ext(e->d_name, ".v64") || has_ext(e->d_name, ".n64")) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = 1;
        }
    }
    if (!found) {
        rewinddir(d);
        while (!found && (e = readdir(d)) != NULL) {
            char p[RC_PATH];
            struct stat st;
            unsigned char h[4];
            FILE *f;
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (stat(p, &st) != 0 || st.st_size != 8 * 1024 * 1024) continue;
            f = fopen(p, "rb");
            if (!f) continue;
            if (fread(h, 1, 4, f) == 4 &&
                ((h[0] == 0x80 && h[1] == 0x37) || (h[0] == 0x37 && h[1] == 0x80) ||
                 (h[0] == 0x40 && h[1] == 0x12))) {
                snprintf(out, outsz, "%s", p);
                found = 1;
            }
            fclose(f);
        }
    }
    closedir(d);
    return found;
}

static int find_nd_wad(const char *dir, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (!strcasecmp(e->d_name, "doom64.wad")) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ */
/* Audio: los tres archivos del WESS (el driver de sonido del N64) tal   */
/* cual estan en la ROM. Los toca source/wess al arrancar.               */
/* ------------------------------------------------------------------ */
#define AUD_WMD_SIZE 0xB9E0      /* instrumentos ("SN64") */
#define AUD_WSD_SIZE 0x142F8     /* secuencias ("SSEQ")   */
#define AUD_WDD_SIZE 0x1716C4    /* muestras              */

static int audio_complete(const char *dir)
{
    return exists(dir, "doom64.wmd") && exists(dir, "doom64.wsd") && exists(dir, "doom64.wdd");
}

static int write_file(const char *dir, const char *name, const unsigned char *p, size_t n)
{
    char fn[512];
    FILE *f;
    size_t w;
    snprintf(fn, sizeof(fn), "%s/%s", dir, name);
    f = fopen(fn, "wb");
    if (!f) return -1;
    w = fwrite(p, 1, n, f);
    if (fclose(f) != 0 || w != n) { remove(fn); return -1; }
    return 0;
}

/* 0 = ok. Busca el WMD por su firma (vale para la 1.0 y la 1.1): el WSD
 * va pegado atras y el WDD despues, alineado a 16. */
static int extract_audio(const char *romfn, const char *dir)
{
    FILE *f = fopen(romfn, "rb");
    unsigned char *rom;
    long n;
    size_t i, wmd = 0, wsd, wdd, wddsz;
    int ok = -1;

    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0x400000 || n > 0x4000000) { fclose(f); return -1; }
    rom = (unsigned char *)malloc((size_t)n);
    if (!rom) { fclose(f); return -1; }
    if (fread(rom, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(rom); return -1; }
    fclose(f);

    /* al orden de bytes del N64 (z64) */
    if (rom[0] == 0x37 && rom[1] == 0x80) {                 /* v64 */
        for (i = 0; i + 1 < (size_t)n; i += 2) { unsigned char t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
    } else if (rom[0] == 0x40 && rom[1] == 0x12) {          /* n64 */
        for (i = 0; i + 3 < (size_t)n; i += 4) {
            unsigned char a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
        }
    }

    for (i = 0x1000; i + AUD_WMD_SIZE + 4 <= (size_t)n; i += 16) {
        if (!memcmp(rom + i, "SN64", 4) && !memcmp(rom + i + AUD_WMD_SIZE, "SSEQ", 4)) { wmd = i; break; }
    }
    if (!wmd) {
        ps3_log("[rom] audio: no encuentro el WMD/WSD en la ROM");
        free(rom);
        return -1;
    }
    wsd = wmd + AUD_WMD_SIZE;
    wdd = (wsd + AUD_WSD_SIZE + 15) & ~(size_t)15;
    wddsz = AUD_WDD_SIZE;
    if (wdd >= (size_t)n) { free(rom); return -1; }
    if (wdd + wddsz > (size_t)n) wddsz = (size_t)n - wdd;
    ps3_logf("[rom] audio: WMD 0x%x  WSD 0x%x  WDD 0x%x (%u bytes)",
             (unsigned)wmd, (unsigned)wsd, (unsigned)wdd, (unsigned)wddsz);

    if (write_file(dir, "doom64.wmd", rom + wmd, AUD_WMD_SIZE) == 0 &&
        write_file(dir, "doom64.wsd", rom + wsd, AUD_WSD_SIZE) == 0 &&
        write_file(dir, "doom64.wdd", rom + wdd, wddsz) == 0)
        ok = 0;
    free(rom);
    return ok;
}

/* Pantalla de error: se queda hasta que el usuario sale con el boton PS. */
static void fatal_screen(const char *const *lines, int n)
{
    ps3_log("[rom] no se puede seguir sin los datos del juego; esperando salida");
    for (;;) {
        PS3_Video_TextScreen("DOOM 64", lines, n, -1);
        sysUtilCheckCallback();
        if (PS3_Plat_ExitRequested()) return;
        usleep(100000);
    }
}

/* ------------------------------------------------------------------ */
/* Punto de entrada: 0 = datos listos, -1 = no se puede seguir          */
/* ------------------------------------------------------------------ */
int PS3_RomConv_Check(const char *dir)
{
    char rom[RC_PATH], wad[RC_PATH], p[RC_PATH];
    u64 t0;
    int r;

    snprintf(p, sizeof(p), "%s/maps", dir); mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/tex", dir);  mkdir(p, 0777);

    if (rom_data_complete(dir)) {
        ps3_log("[rom] datos del juego presentes, no hace falta la ROM");
    } else {
        if (!find_rom(dir, rom, sizeof(rom))) {
            static const char *const msg[] = {
                "Game data not found.",
                "",
                "Copy your Doom 64 ROM (USA, .z64 / .v64 / .n64),",
                "UNZIPPED (a .zip will not work), to the game folder:",
                "  /dev_hdd0/game/DOOM64CEL/USRDIR/",
                "",
                "Optional: DOOM64.WAD from the remaster (Lost Levels).",
                "",
                "To exit: PS button -> Quit Game",
            };
            ps3_log("[rom] FALTAN los datos del juego y no hay ROM en USRDIR");
            fatal_screen(msg, sizeof(msg) / sizeof(msg[0]));
            return -1;
        }
        ps3_logf("[rom] convirtiendo %s", rom);

        snprintf(p, sizeof(p), "%s/%s", dir, ROM_MARK_RUN);
        { FILE *f = fopen(p, "w"); if (f) { fputs(rom, f); fclose(f); } }

        s_mode_lost = 0;
        t0 = now_us();
        r = PS3_RomConv_Rom(rom, dir);
        if (r != 0) {
            static const char *const msg[] = {
                "The ROM could not be converted.",
                "",
                "It must be Doom 64 for N64, USA version (1.0 or 1.1).",
                "Details are in USRDIR/doom64_log.txt",
                "",
                "To exit: PS button -> Quit Game",
            };
            ps3_log("[rom] ERROR en la conversion (ver lineas [rom] de arriba)");
            fatal_screen(msg, sizeof(msg) / sizeof(msg[0]));
            return -1;
        }
        remove(p);
        ps3_logf("[rom] conversion de la ROM OK en %u segundos",
                 (unsigned)((now_us() - t0) / 1000000));
    }

    /* Audio (musica y efectos): tambien en instalaciones que ya tenian los
     * datos convertidos de antes, si la ROM sigue en USRDIR. */
    if (!audio_complete(dir)) {
        if (find_rom(dir, rom, sizeof(rom))) {
            s_mode_lost = 0;
            s_step = 7;
            s_what = "music and sound";
            draw_progress(99);
            if (extract_audio(rom, dir) == 0)
                ps3_log("[rom] audio de la ROM extraido");
            else
                ps3_log("[rom] ERROR extrayendo el audio de la ROM: se sigue sin el");
        } else {
            ps3_log("[rom] sin audio de la ROM (falta doom64.wmd/wsd/wdd y no hay ROM)");
        }
    }

    /* Lost Levels: solo si falta alguno y esta el WAD del remaster. */
    if (!lost_complete(dir)) {
        if (find_nd_wad(dir, wad, sizeof(wad))) {
            ps3_logf("[rom] convirtiendo Lost Levels desde %s", wad);
            s_mode_lost = 1;
            s_what = "reading the WAD";
            draw_progress(-1);
            t0 = now_us();
            r = PS3_RomConv_Lost(wad, dir);
            if (r < 0)
                ps3_log("[rom] ERROR con el WAD del remaster: se sigue sin Lost Levels");
            else
                ps3_logf("[rom] Lost Levels: %d de 7 mapas en %u segundos", r,
                         (unsigned)((now_us() - t0) / 1000000));
        } else {
            ps3_log("[rom] sin DOOM64.WAD del remaster: sin Lost Levels");
        }
    } else {
        ps3_log("[rom] Lost Levels presentes");
    }
    return 0;
}
