/* ps3_data.h -- carga y validacion de los datos del juego. */

#ifndef PS3_DATA_H
#define PS3_DATA_H

/* Carga los WAD desde basedir y loguea lo que encuentra.
 * basedir es normalmente PS3_USRDIR. */
void PS3_Data_Probe(const char *basedir);

#endif /* PS3_DATA_H */
