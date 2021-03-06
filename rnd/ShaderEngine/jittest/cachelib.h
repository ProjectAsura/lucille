#ifndef LUCILLE_CACHE_LIB_H
#define LUCILLE_CACHE_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "hbuffer.h"

#define MAX_LAYER 32



#define N_LAYER   29            /* Normal   */
#define UV_LAYER  30            /* UV, t    */
#define P_LAYER   31            /* Position */

extern ri_hbuffer_t *g_hbuffer[MAX_LAYER];

extern void lse_init(
    int w,
    int h);

extern void lse_save_cache_iiic(
    int     layer,
    int     x,
    int     y,
    float  *val);

extern void lse_load_cache_iiic(
    int     layer,
    int     x,
    int     y,
    float  *val);

#ifdef __cplusplus
}
#endif

#endif  /* LUCILLE_CACHE_LIB_H */
