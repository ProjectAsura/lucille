/*
 * texture data loader.
 *
 * TODO:
 * 
 * To handle large texture maps,
 * a imagemap(e.g. jpeg) is first converted into blocked,
 * mipmapped and hierarchical manner, and saved it to disk.
 * In rendering time, a portion(block) of texture map to be texture-mapped
 * is load into memory from file.
 *
 *
 *  +-----------------+         +-----+-----+-----+
 *  |                 |         |     |     |     |
 *  |                 |         |     |     |     |
 *  |  input texture  |      -> +-----+-----+-----+ 
 *  |                 |         |     |     |     |
 *  |                 |         |     |     |     |
 *  +-----------------+         +-----+-----+-----+
 *                                          <----->
 *                                           TEXBLOCKSIZE
 *
 *
 * -> Generate mipmaps and store it to disk as a 1D array manner.
 *
 * +-----------------------+---------------+------------+--||--+ 
 * | miplevel 0 blocks     | lv 1 blks     | lv 2 blks  |      |
 * +-----------------------+---------------+------------+--||--+
 *
 *
 *  o Use rip-map for anisotropic texturing.
 *
 *
 * $Id$
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>


#ifdef HAVE_ZLIB
#include <zlib.h>        /* Blocked texture is saved with zlib comporession. */
#endif

#include "memory.h"
#include "log.h"
#include "texture.h"
#include "hash.h"
#include "render.h"
#include "image_loader.h"       /* ../imageo                                */

#define TEXBLOCKSIZE 64         /* block map size in miplevel 0. */
#define MAXMIPLEVEL  16         /* 16 can represent a mipmap for 65536x65536. */

typedef struct _texblock_t
{
    int          x, y;        /* upper-left position in original texture map. 
                               * (in miplevel 0)
                               */

    ri_vector_t *image;

} texblock_t;

typedef struct _blockedmipmap_t
{
    int nmiplevels;             /* The number of mipmap levels              */

    int width, height;          /* Original texture size                    */

    int nxblocks, nyblocks;     /* The number of texture blocks.
                                 * Counted in miplevel 0.
                                 * Thus e.g. miplevel 1 contains
                                 * (nxblocks/2) * (nyblocks/2) texture blocks.
                                 */

    texblock_t *blocks[MAXMIPLEVEL];    /* list of texture blocks
                                         * in each miplevel.
                                         */

} blockedmipmap_t;

//#define LOCAL_DEBUG
#undef  LOCAL_DEBUG
#define USE_ZORDER 0

#define MAX_TEXTURE_PATH 1024

static ri_hash_t    *texture_cache;

/* input x and y must be up to 2^16 */
#define MAP_Z2D(x, y) ((unsigned int)(g_z_table[(x) & 0xFF] | \
               (g_z_table[(y) & 0xFF] << 1)) | \
               ((unsigned int)((g_z_table[((x) >> 8) & 0xFF]) | \
                (g_z_table[((y) >> 8) & 0xFF] << 1)) << 16))

#if USE_ZORDER
/* table for z curve order */
static unsigned short g_z_table[256];

/* store texel memory in scanline order -> z curve order. */ 
static void          remapping(ri_texture_t *src);
static void          build_z_table();
#endif  /* USE_ZORDER */


#if 0 /* REMOVE */
static void endconv(void *data)
{
#if defined(WORDS_BIGENDIAN) || defined(__APPLE__)
    char tmp[4];
    tmp[0] = ((char *)data)[0];
    tmp[1] = ((char *)data)[1];
    tmp[2] = ((char *)data)[2];
    tmp[3] = ((char *)data)[3];

    ((char *)data)[0] = tmp[3];
    ((char *)data)[1] = tmp[2];
    ((char *)data)[2] = tmp[1];
    ((char *)data)[3] = tmp[0];
#else
    (void)data;
#endif
}
#endif

ri_texture_t *
ri_texture_load(const char *filename)
{
    static int      initialized = 0;
    ri_texture_t   *p;
    char            fullpath[4096];

    if ((filename == '\0') || (strlen(filename) == 0)) {
        ri_log(LOG_WARN, "(TexLdr) Null input texture filename.");
        return NULL;
    }

    if (!initialized) {
        texture_cache = ri_hash_new();
#if USE_ZORDER
        build_z_table();
#endif
        initialized = 1;
    }

    if (ri_hash_lookup(texture_cache, filename)) {
        /* hit texture cache! */
        p = (ri_texture_t *)ri_hash_lookup(texture_cache, filename);

        return p;
    }

    if (ri_render_get() == NULL) {

        /* The function is called before the renderer is initialized.
         * Disable file search.
         */

        ri_log(LOG_WARN, "(TexLdr) ri_texture_load was called before initializing the renderer. Finding a texture file from search path is disabled.\n");

        strcpy(fullpath, filename);
    
    } else {

        if (!ri_option_find_file(fullpath,
                                 ri_render_get()->context->option,
                                 filename)) {
            ri_log(LOG_FATAL, "(TexLdr) Can't find textue file \"%s\"", filename);
            ri_log(LOG_FATAL, "(TexLdr) Searched from following path.");
            ri_option_show_searchpath(ri_render_get()->context->option);
            exit(-1);
        }
    
    }

    {
        unsigned int  width;
        unsigned int  height;
        unsigned int  component;
        float        *image = NULL;

        image = ri_image_load(fullpath, &width, &height, &component);
        if (!image) {
            ri_log(LOG_WARN, "(TexLdr) Can't load textue file \"%s\"", fullpath);
            exit(-1);
        }    

        p = ri_mem_alloc(sizeof(ri_texture_t));
        assert(p != NULL);
        memset(p, 0, sizeof(ri_texture_t));
        
        p->width  = width;
        p->height = height;
        p->data   = image;

        ri_log(LOG_INFO, "(TexLdr) Loaded texture [ %s ] size = %d x %d", fullpath, width, height);
    }


#if USE_ZORDER
    /* reorder texel memory with z curve order to exploit cache coherency. */
    remapping(p);
#endif

    /* add to texture cache */
    ri_hash_insert(texture_cache, filename, p);

    return p;
}

void
ri_texture_free(ri_texture_t *texture)
{
    ri_mem_free(texture->data);
    ri_mem_free(texture);
}


#if USE_ZORDER

static void
build_z_table()
{
    unsigned int   i, j;
    unsigned int   bit;
    unsigned int   v;
    unsigned int   ret;
    unsigned int   mask = 0x1;
    int            shift;

    for (i = 0; i < 256; i++) {
        v = i;
        shift = 0;
        ret = 0;
        for (j = 0; j < 8; j++) {
            /* extract (j+1)'th bit */
            bit = (v >> j) & mask;

            ret += bit << shift;
            shift += 2;
        }

        g_z_table[i] = (unsigned short)ret;
    }
}

static void
remapping(ri_texture_t *src)
{
    unsigned int    x, y;
    unsigned int    i;
    unsigned int    pow2n;
    unsigned int    maxpixlen;
    unsigned int    idx;
    float          *newdata;

    if (src->width > src->height) {
        maxpixlen = src->width;
    } else {
        maxpixlen = src->height;
    } 

    /* find nearest 2^n value against maxpixlen.
     * I think there exists more sofisticated way for this porpose ...
     */
    pow2n = 1;
    for (i = 1; i < maxpixlen; i++) {
        if (maxpixlen <= pow2n) break;
        pow2n *= 2;
    }
    
    src->maxsize_pow2n = pow2n;

#ifdef LOCAL_DEBUG
    fprintf(stderr, "width = %d, height = %d\n", src->width, src->height);
    fprintf(stderr, "maxsize_pow2n = %d\n", src->maxsize_pow2n);
#endif

    newdata = (float *)ri_mem_alloc(4 * sizeof(float) *
                    src->maxsize_pow2n *
                    src->maxsize_pow2n);

    if (src->maxsize_pow2n > 65536) {
        fprintf(stderr,
            "texture size must be < 65536. maxsize_pow2n = %d\n",
            src->maxsize_pow2n);
        exit(-1);
    }

    for (i = 0;
         i < (unsigned int)(4 * src->maxsize_pow2n * src->maxsize_pow2n);
         i++) {
        newdata[i] = 0.0;
    }

    for (y = 0; y < (unsigned int)src->height; y++) {
        for (x = 0; x < (unsigned int)src->width; x++) {
            /* compute z curve order index for (x, y). */
            idx = MAP_Z2D(x, y);
#ifdef LOCAL_DEBUG
            fprintf(stderr, "(%d, %d) = %d\n", x, y, idx);
#endif

            newdata[4 * idx + 0] =
                src->data[4 *(y * src->width + x) + 0];
            newdata[4 * idx + 1] =
                src->data[4 *(y * src->width + x) + 1];
            newdata[4 * idx + 2] =
                src->data[4 *(y * src->width + x) + 2];
            newdata[4 * idx + 3] =
                src->data[4 *(y * src->width + x) + 3];
        }
    }    

    ri_mem_free(src->data);

    src->data = newdata;
}
#endif  /* USE_ZORDER */

void
make_texture(ri_rawtexture_t *rawtex)
{
    int s, t;
    int i;
    int blocksize = 0;

    int nxblocks = 0;    // Number of blocks in x-direction
    int nyblocks = 0;    // Number of blocks in y-direction

    ri_vector_t *image[MAXMIPLEVEL];    // block map images.
    
    for (i = 0; i < blocksize; i++) {
        blocksize = TEXBLOCKSIZE >> i;
        image[i] = (ri_vector_t *)ri_mem_alloc(sizeof(ri_vector_t) *
                blocksize * blocksize);
    }

    nxblocks = (int)ceil(rawtex->width / TEXBLOCKSIZE);
    nyblocks = (int)ceil(rawtex->height / TEXBLOCKSIZE);

    printf("w, h = %d, %d. nxblk, nyblk = %d, %d\n",
        rawtex->width, rawtex->height, nxblocks, nyblocks);

    for (t = 0; t < nyblocks; t++) {
        for (s = 0; s < nxblocks; s++) {

        }
    }
    
    
}

//
// Simple 2x2 -> 1x1 minification.
// TODO: Add sophisticated filtering when calculating mipmap.
//
static void gen_mipmap(float *dst, float *src, int srcw, int srch)
{
    
    int k;
    int w, h;
    int dstw, dsth;

    dstw = srcw / 2;
    dsth = srch / 2;

    ri_float_t val[4];

    for (h = 0; h < dsth; h++) {
        for (w = 0; w < dstw; w++) {
            for (k = 0; k < 4; k++) {   /* RGBA */
                val[0] = src[4 * ((2 * h + 0) * srcw + (2 * w + 0)) + k];
                val[1] = src[4 * ((2 * h + 0) * srcw + (2 * w + 1)) + k];
                val[2] = src[4 * ((2 * h + 1) * srcw + (2 * w + 0)) + k];
                val[3] = src[4 * ((2 * h + 1) * srcw + (2 * w + 1)) + k];

                dst[4 * (h * dstw + w) + k] =
                    0.25 * (val[0] + val[1] + val[2] + val[3]);
            }
        }
    }


}

/*
 * TODO: Consider moving mipmap gen source into another file.
 *       texture_loader.c should contain only source codes for loading
 *       texture resources.
 */
ri_mipmap_t *
ri_texture_make_mipmap(
    const ri_texture_t *texture)
{
    int max_size;
    int nlevels;

    ri_mipmap_t *mipmap;

    if (texture->width > texture->height) {
        max_size = texture->width;
    } else {
        max_size = texture->height;
    }

    /*
     * 1. Calculate the number of mipmap levels.
     */
    {
        int size     = max_size;
        nlevels      = 0;

        while (size < 2) {
            nlevels++;
            size /= 2;
        }
    }

    assert(nlevels < RI_MAX_MIPMAP_SIZE);

    /*
     * 2. Calculate mipmaps
     */
    {
        int      i;
        int      w, h;
        float   *src;

        mipmap  = (ri_mipmap_t *)ri_mem_alloc(sizeof(ri_mipmap_t));
        w       = texture->width;
        h       = texture->height;

        mipmap->width[0]    = w;
        mipmap->height[0]   = h;
        mipmap->data[0]     = ri_mem_alloc(sizeof(float) * w * h * 4);
        memcpy(&mipmap->data[0], texture->data, sizeof(float) * w * h * 4);

        for (i = 1; i < nlevels; i++) {

            w /= 2; h /= 2;

            mipmap->width[i]    = w;
            mipmap->height[i]   = h;
            mipmap->data[i]     = ri_mem_alloc(sizeof(float) * w * h * 4);

            src = mipmap->data[i-1];

            gen_mipmap(mipmap->data[i], src, w, h);

        }

    }

    return mipmap;

}
    

ri_sat_t *
ri_texture_make_sat(
    const ri_texture_t *texture)
{

    ri_sat_t *sat;

    /*
     * 1. Allocate resources for SAT.
     */
    { 
        sat = (ri_sat_t *)ri_mem_alloc(sizeof(ri_sat_t));

        sat->data = (double *)ri_mem_alloc(
                        sizeof(double) * texture->width * texture->height * 4);

        sat->width  = texture->width;
        sat->height = texture->height;
    }

    /*
     * Calculate SAT.
     * TODO: Acturally, SAT image have (w+1) x (h+1) resolution.
     */
    {
        int k;
        int i, j;
        int w = texture->width;
        int h = texture->height;
        
        
        /*
         * 1. Horizontal
         */

        for (j = 0; j < h; j++) {

            for (k = 0; k < 4; k++) {
                sat->data[4 * (j * w) + k] = texture->data[4 * (j * w) + k];
            }

            for (i = 1; i < w; i++) {

                for (k = 0; k < 4; k++) {
                    sat->data[4 * (j * w + i) + k] =
                          sat->data[4 * (j * w + (i-1)) + k]
                        + texture->data[4 * (j * w + i) + k];
                }

            }

        }

        /*
         * 2. Vertical
         */

        for (j = 1; j < h; j++) {

            for (i = 0; i < w; i++) {

                for (k = 0; k < 4; k++) {
                    sat->data[4 * (j * w + i) + k] =
                          sat->data[4 * ((j-1) * w + i) + k]
                        + sat->data[4 * (j * w + i) + k];
                }

            }

        }
        

    }

    return sat;
}

static void
normalize3(double v[3])
{
    double len;

    len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len >= 1.0e-6) {
        len = 1.0 / len;
        v[0] *= len;
        v[1] *= len;
        v[2] *= len;
    }

}

static void
longtitude_latitude2xyz(
    double xyz[3],
    double uv[2])
{
    double phi, theta;

    theta = uv[1] * M_PI;
    phi   = uv[0] * 2.0 * M_PI;

    xyz[0] =  sin(theta) * cos(phi);
    xyz[1] =  -cos(theta);
    xyz[2] =  sin(theta) * sin(phi);
}

static void
fetch_angular_map_from_xyz(
    float           col[3],
    double          xyz[3],
    const float    *img,
    int             width,
    int             height)
{
    int    t, s; 
    double r;
    double norm2;
    double u, v;

    normalize3(xyz);

    r = (1.0 / M_PI) * acos(xyz[2]);
    norm2 = xyz[0] * xyz[0] + xyz[1] * xyz[1];
    if (norm2 > 1.0e-6) r /= sqrt(norm2);

    u = xyz[0] * r;
    v = xyz[1] * r;

    u = 0.5 * u + 0.5;
    v = 0.5 * v + 0.5;

    t = u * width;
    s = v * height;

    col[0] = img[4 * (s * width + t) + 0];
    col[1] = img[4 * (s * width + t) + 1];
    col[2] = img[4 * (s * width + t) + 2];
}

static void
angular_map2longtitude_latitude(
    float       *dst,             /* long-lat coord   */
    const float *src,             /* anular map coord */
    int          dstw,
    int          dsth,
    int          srcw,
    int          srch)
{
    int i, j;
    double xyz[3];
    double uv[2];
    float  col[3];

    for (j = 0; j < dsth; j++) {
        for (i = 0; i < dstw; i++) {
            uv[0] = i / (double)dstw;
            uv[1] = j / (double)dsth;

            longtitude_latitude2xyz(xyz, uv);

            fetch_angular_map_from_xyz(col, xyz, src, srcw, srch);

            dst[4 * (j * dstw + i) + 0] = col[0];
            dst[4 * (j * dstw + i) + 1] = col[1];
            dst[4 * (j * dstw + i) + 2] = col[2];
            dst[4 * (j * dstw + i) + 3] = 1.0f;
        }
    }
}

ri_texture_t *
ri_texture_make_longlat_from_angularmap(
    ri_texture_t *texture,      /* angular map texture  */
    int           longlat_width,
    int           longlat_height)
{
    ri_texture_t *dst;

    dst = (ri_texture_t *)ri_mem_alloc(sizeof(ri_texture_t));
    dst->width  = longlat_width;
    dst->height = longlat_height;
    dst->data   = (float *)ri_mem_alloc(
                    sizeof(float) * longlat_width * longlat_height * 4);

    angular_map2longtitude_latitude(
        dst->data, texture->data,
        dst->width, dst->height, texture->width, texture->height);
    
    return dst;
}

#if 0   // TODO
// Generate blocked mipmap from raw texture map.
// TODO: implement.
static blockedmipmap_t *
gen_blockedmipmap(const char *filename, ri_rawtexture_t *texture)
{
    int i;
    int w, h;
    int xblocks, yblocks;
    int miplevels;
    int minsize;
    int u, v;

    w = texture->width; h = texture->height;

    xblocks = (int)ceil(w / TEXBLOCKSIZE);
    yblocks = (int)ceil(h / TEXBLOCKSIZE);

    ri_log(LOG_INFO, "texsize = (%d, %d). blocks = (%d, %d)\n",
        w, h, xblocks, yblocks);

    minsize = (w < h) ? w : h;
    miplevels = (int)ceil(log((ri_float_t)minsize) / log(2.0));

    ri_log(LOG_INFO, "miplevels = %d\n", miplevels);

    for (i = 0; i < miplevels; i++) {

        for (v = 0; v < yblocks; v++) {
            for (u = 0; u < xblocks; u++) {
                
            }
        }

    }
    
    return NULL;    // not yet implemented.
}

// Write mipmap to disk with zlib compression.
static void
write_blockedmipmap(const char *filename, blockedmipmap_t *blkmipmap)
{
    int i, j;
    int u, v;
    int size;
    int xblocks, yblocks;

    gzFile *fp;
    
    fp = gzopen(filename, "wb");
    if (!fp) {
        
        ri_log(LOG_ERROR, "Can't write file [%s]", filename);
    }

    // Write header
    gzwrite(fp, &blkmipmap->nmiplevels, sizeof(int));
    gzwrite(fp, &blkmipmap->width, sizeof(int));
    gzwrite(fp, &blkmipmap->height, sizeof(int));
    gzwrite(fp, &blkmipmap->nxblocks, sizeof(int));
    gzwrite(fp, &blkmipmap->nyblocks, sizeof(int));
    
    size = sizeof(ri_float_t) * TEXBLOCKSIZE * TEXBLOCKSIZE;
    
    for (i = 0; i < blkmipmap->nmiplevels; i++) {

        xblocks = blkmipmap->nxblocks >> i;
        yblocks = blkmipmap->nyblocks >> i;

        for (v = 0; v < yblocks; v++) {
            for (u = 0; u < xblocks; u++) {

                j = v * xblocks + u;

                // Write texture block.
                gzwrite(fp, blkmipmap->blocks[i][j].image,
                    size);

            }
        }

    }

    gzclose(fp);
}
#endif
