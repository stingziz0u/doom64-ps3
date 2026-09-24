#include <stdio.h>
#include "ps3_game.h"
void PS3_Engine_Probe(const char *basedir);
void ps3_log_init(void);
void ps3_log_shutdown(void);
void soft_save(const char *path);
extern int fake_tris;
static void run(ps3_input_t *in, int frames) { for (int i = 0; i < frames; i++) { PS3_Game_Frame(in); } }
static void shot(const char *name) { PS3_Game_Draw(); soft_save(name); printf("%s\n", name); }
int main(void)
{
    ps3_input_t in = {0};
    ps3_log_init();
    PS3_Engine_Probe("/dev_hdd0/game/DOOM64CEL/USRDIR");
    run(&in, 2); shot("out/f0_inicio.ppm");
    in.rx = 100; run(&in, 30); in.rx = 0; shot("out/f1_giro.ppm");
    in.ly = -120; run(&in, 60); in.ly = 0; shot("out/f2_avance.ppm");
    in.rx = -100; run(&in, 50); in.rx = 0; in.ly = -120; run(&in, 80); in.ly = 0; shot("out/f3_otra.ppm");
    in.buttons = 0x0100; run(&in, 2); shot("out/f4_mapa.ppm"); in.buttons = 0; run(&in,2);
    PS3_Game_Shutdown();
    ps3_log_shutdown();
    return 0;
}
