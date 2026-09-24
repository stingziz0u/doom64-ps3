/* ps3_game.h -- la interfaz entre la capa de plataforma (main.c) y el juego.
 *
 * Este header NO incluye ni los headers del motor ni ps3_pad.h, a proposito:
 * los dos definen PAD_UP, PAD_LEFT, PAD_START... con valores distintos
 * (el motor usa los bits del mando de N64, ps3_pad.h los del DualShock 3).
 * La traduccion vive en ps3_game.c, que ve el motor, y recibe el mando
 * como datos crudos en esta struct.
 */

#ifndef PS3_GAME_H
#define PS3_GAME_H

/* Estado del DualShock 3 tal como lo deja ps3_pad.c. */
typedef struct {
    unsigned short buttons;     /* bitmask DS3_* */
    int lx, ly;                 /* stick izquierdo, -128..127, deadzone aplicada */
    int rx, ry;                 /* stick derecho */
} ps3_input_t;

/* Mismos valores que ps3_pad.h, con otro nombre para no chocar con el motor. */
#define DS3_LEFT      0x8000
#define DS3_DOWN      0x4000
#define DS3_RIGHT     0x2000
#define DS3_UP        0x1000
#define DS3_START     0x0800
#define DS3_R3        0x0400
#define DS3_L3        0x0200
#define DS3_SELECT    0x0100
#define DS3_SQUARE    0x0080
#define DS3_CROSS     0x0040
#define DS3_CIRCLE    0x0020
#define DS3_TRIANGLE  0x0010
#define DS3_R1        0x0008
#define DS3_L1        0x0004
#define DS3_R2        0x0002
#define DS3_L2        0x0001

/* Arranque: juego nuevo en 'map' (G_InitNew + P_SetupLevel). */
void PS3_Game_NewGame(int map);

/* Prueba sin mando: 'ntics' tics con botones en 0 (etapa 2b). */
int  PS3_Game_RunTics(int ntics);

/* 1 si hay un nivel cargado y listo para correr. */
int  PS3_Game_Ready(void);

/* Llamar UNA vez por frame de video (60 Hz). Corre un tic cada dos frames
 * (el juego es de 30 Hz) con el mando traducido, y maneja muerte / fin de
 * nivel recargando lo que corresponda. */
void PS3_Game_Frame(const ps3_input_t *in);

/* Color de fondo mientras no hay renderer: el color de luz del sector donde
 * esta parado el jugador, con flash rojo al recibir dano y dorado al agarrar
 * items. Formato 0xAARRGGBB, listo para PS3_Video_Clear. */
unsigned int PS3_Game_ViewColor(void);

/* Dibuja un frame con el renderer del motor (P_Drawer). El emulador del TA
 * lo manda al RSX; llamar entre PS3_Video_BeginFrame y PS3_Video_EndFrame. */
void PS3_Game_Draw(void);

/* Cierre del nivel (P_Stop). */
void PS3_Game_Shutdown(void);

#endif /* PS3_GAME_H */
