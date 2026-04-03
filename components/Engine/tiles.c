//
//  tiles.c
//  Duke3D
//
//  Created by fabien sanglard on 12-12-22.
//  Copyright (c) 2012 fabien sanglard. All rights reserved.
//

#include "tiles.h"
#include "engine.h"
#include "draw.h"
#include "filesystem.h"
#include "tilecache.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#ifdef DUKE3D_TILE_ASYNC_SPIKE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#endif

// Per-frame diagnostic counters — read and reset by spi_lcd_send_boarder() each frame.
volatile int32_t diag_tile_loads = 0;   // number of loadtile() calls this frame
volatile int32_t diag_tile_bytes = 0;   // bytes read from SD this frame
volatile int64_t diag_tile_us    = 0;   // microseconds spent in kread() this frame

#ifdef DUKE3D_TILE_ASYNC_SPIKE
static SemaphoreHandle_t g_tile_sd_mutex;
static int g_tile_sd_mutex_ready;
volatile int tile_async_spike_runtime_enabled = 1;
static QueueHandle_t g_tile_async_q;
enum { TILE_ASYNC_QUEUE_LEN = 64 };
static void tile_async_worker_main(void *arg);
#endif

char  artfilename[20];

/* Static PSRAM BSS for tile metadata — avoids depending on a large heap slab. */
EXT_RAM_ATTR static tile_t tiles_psram[MAXTILES];
tile_t *tiles = tiles_psram;

/* Separate per-tile texture data pointers. tile_t::data was removed to shrink
   tile_t from 16→12 bytes. waloff in PSRAM BSS (9216 * 4 = 36,864 bytes) to
   keep internal DRAM free for WiFi driver (~100KB needed). */
EXT_RAM_ATTR uint8_t* waloff[MAXTILES];

int32_t numTiles;

int32_t artversion;

uint8_t  *pic = NULL;

EXT_RAM_ATTR uint8_t  gotpic[(MAXTILES+7)>>3];

void setviewtotile(short tilenume, int32_t tileWidth, int32_t tileHeight)
{
    int32_t i, j;
    
    /* DRAWROOMS TO TILE BACKUP&SET CODE */
    tiles[tilenume].dim.width = tileWidth;
    tiles[tilenume].dim.height = tileHeight;
    bakxsiz[setviewcnt] = tileWidth;
    bakysiz[setviewcnt] = tileHeight;
    bakvidoption[setviewcnt] = vidoption;
    vidoption = 2;
    bakframeplace[setviewcnt] = frameplace;
    frameplace = waloff[tilenume];
    bakwindowx1[setviewcnt] = windowx1;
    bakwindowy1[setviewcnt] = windowy1;
    bakwindowx2[setviewcnt] = windowx2;
    bakwindowy2[setviewcnt] = windowy2;
    copybufbyte(&startumost[windowx1],&bakumost[windowx1],(windowx2-windowx1+1)*sizeof(bakumost[0]));
    copybufbyte(&startdmost[windowx1],&bakdmost[windowx1],(windowx2-windowx1+1)*sizeof(bakdmost[0]));
    setview(0,0,tileHeight-1,tileWidth-1);
    setaspect(65536,65536);
    j = 0;
    for(i=0; i<=tileWidth; i++) {
        ylookup[i] = j;
        j += tileWidth;
    }
    setBytesPerLine(tileHeight);
    setviewcnt++;
}




void squarerotatetile(short tilenume)
{
    int32_t i, j, k;
    uint8_t  *ptr1, *ptr2;
    
    dimensions_t tileDim;
    
    tileDim.width = tiles[tilenume].dim.width;
    tileDim.height = tiles[tilenume].dim.height;
    
    /* supports square tiles only for rotation part */
    if (tileDim.width == tileDim.height)
    {
        k = (tileDim.width<<1);
        for(i=tileDim.width-1; i>=0; i--)
        {
            ptr1 = waloff[tilenume]+i*(tileDim.width+1);
            ptr2 = ptr1;
            if ((i&1) != 0) {
                ptr1--;
                ptr2 -= tileDim.width;
                swapchar(ptr1,ptr2);
            }
            for(j=(i>>1)-1; j>=0; j--)
            {
                ptr1 -= 2;
                ptr2 -= k;
                swapchar2(ptr1,ptr2,tileDim.width);
            }
        }
    }
}



//1. Lock a picture in the cache system.
//2. Mark it as used in the bitvector tracker.
IRAM_ATTR void setgotpic(int32_t tilenume)
{
    if (tiles[tilenume].lock < 200)
        tiles[tilenume].lock = 199;
    
    gotpic[tilenume>>3] |= pow2char[tilenume&7];
}





// GRP path: load natively up to DUKE3D_TILE_MAX_NATIVE_EDGE (same as TCACHE.BIN);
// larger tiles are halved until within that limit. tiles[].dim stays at ART
// dimensions; picsiz is updated when downscaling so drawing uses the right mask.
#define MAX_TILE_DIM DUKE3D_TILE_MAX_NATIVE_EDGE

/* GRP seek + read (+ optional downscale) into existing waloff[]. Used by sync loadtile
 * and async worker. Skips TILECACHE and allocache. Caller must hold tile SD mutex (spike). */
static void loadtile_grp_read_into_slot(short tilenume)
{
    int32_t i, tileFilesize;
    int32_t orig_w = tiles[tilenume].dim.width;
    int32_t orig_h = tiles[tilenume].dim.height;
    tileFilesize = orig_w * orig_h;
    if (tileFilesize <= 0 || waloff[tilenume] == NULL) {
        tiles[tilenume].lock = 199;
        return;
    }

    int32_t new_w = orig_w, new_h = orig_h;
    while (new_w > MAX_TILE_DIM) new_w >>= 1;
    while (new_h > MAX_TILE_DIM) new_h >>= 1;
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;

    int32_t do_scale = (new_w != orig_w || new_h != orig_h);
    uint8_t *tmp_buf = NULL;
    if (do_scale) {
        tmp_buf = (uint8_t *)heap_caps_malloc(tileFilesize,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp_buf) do_scale = 0;
    }

    i = tilefilenum[tilenume];
    if (i != artfilnum) {
        if (artfil != -1)
            kclose(artfil);
        artfilnum = i;
        artfilplc = 0L;

        artfilename[7] = (i % 10) + 48;
        artfilename[6] = ((i / 10) % 10) + 48;
        artfilename[5] = ((i / 100) % 10) + 48;
        artfil = TCkopen4load(artfilename, 0);

        if (artfil == -1) {
            printf("Error, unable to load artfile:'%s'.\n", artfilename);
            getchar();
            exit(0);
        }

        faketimerhandler();
    }

    if (artfilplc != tilefileoffs[tilenume]) {
        klseek(artfil, tilefileoffs[tilenume] - artfilplc, SEEK_CUR);
        faketimerhandler();
    }

    {
        int64_t _t0 = esp_timer_get_time();

        if (!do_scale) {
            kread(artfil, waloff[tilenume], tileFilesize);
        } else {
            kread(artfil, tmp_buf, tileFilesize);
            uint8_t *dst = waloff[tilenume];
            for (int32_t x = 0; x < new_w; x++) {
                int32_t sx = (x * orig_w) / new_w;
                for (int32_t y = 0; y < new_h; y++) {
                    int32_t sy = (y * orig_h) / new_h;
                    dst[x * new_h + y] = tmp_buf[sx * orig_h + sy];
                }
            }
            heap_caps_free(tmp_buf);

            {
                int32_t pw = 0, ph = 0, w = new_w, h = new_h;
                while (w > 1) {
                    pw++;
                    w >>= 1;
                }
                while (h > 1) {
                    ph++;
                    h >>= 1;
                }
                picsiz[tilenume] = (uint8_t)(pw | (ph << 4));
            }
        }

        diag_tile_us += esp_timer_get_time() - _t0;
        diag_tile_loads += 1;
        diag_tile_bytes += tileFilesize;
    }
    faketimerhandler();
    artfilplc = tilefileoffs[tilenume] + tileFilesize;
    tiles[tilenume].lock = 199;
}

static void loadtile_body(short tilenume)
{
    int32_t i, tileFilesize;

    if ((uint32_t)tilenume >= (uint32_t)MAXTILES)
        return;

    int32_t orig_w = tiles[tilenume].dim.width;
    int32_t orig_h = tiles[tilenume].dim.height;
    tileFilesize = orig_w * orig_h;

    if (tileFilesize <= 0)
        return;

    // Fast path: read native-resolution tile from TILECACHE.BIN (one seek + one read).
    // Falls through to the GRP path if tile is absent from the cache.
    if (waloff[tilenume] == NULL) {
        TileCacheHit hit;
        if (tilecache_lookup(tilenume, &hit)) {
            int64_t _t0 = esp_timer_get_time();
            tiles[tilenume].lock = 199;
            allocache(&waloff[tilenume], hit.w * hit.h, (uint8_t *)&tiles[tilenume].lock);
            tilecache_read(hit.off, waloff[tilenume], hit.w * hit.h);
            diag_tile_us += esp_timer_get_time() - _t0;
            diag_tile_loads += 1;
            diag_tile_bytes += hit.w * hit.h;
            int32_t pw = 0, ph = 0, tw = hit.w, th = hit.h;
            while (tw > 1) {
                pw++;
                tw >>= 1;
            }
            while (th > 1) {
                ph++;
                th >>= 1;
            }
            picsiz[tilenume] = (uint8_t)(pw | (ph << 4));
            return;
        }
    }

    int32_t new_w = orig_w, new_h = orig_h;
    while (new_w > MAX_TILE_DIM) new_w >>= 1;
    while (new_h > MAX_TILE_DIM) new_h >>= 1;
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;

    int32_t do_scale = (new_w != orig_w || new_h != orig_h);
    uint8_t *tmp_buf = NULL;
    if (do_scale) {
        tmp_buf = (uint8_t *)heap_caps_malloc(tileFilesize,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp_buf) do_scale = 0;
    }

    int32_t cacheBytes = do_scale ? (new_w * new_h) : tileFilesize;

    i = tilefilenum[tilenume];
    if (i != artfilnum) {
        if (artfil != -1)
            kclose(artfil);
        artfilnum = i;
        artfilplc = 0L;

        artfilename[7] = (i % 10) + 48;
        artfilename[6] = ((i / 10) % 10) + 48;
        artfilename[5] = ((i / 100) % 10) + 48;
        artfil = TCkopen4load(artfilename, 0);

        if (artfil == -1) {
            printf("Error, unable to load artfile:'%s'.\n", artfilename);
            getchar();
            exit(0);
        }

        faketimerhandler();
    }

    if (waloff[tilenume] == NULL) {
        tiles[tilenume].lock = 199;
        allocache(&waloff[tilenume], cacheBytes, (uint8_t *)&tiles[tilenume].lock);
    }

    if (artfilplc != tilefileoffs[tilenume]) {
        klseek(artfil, tilefileoffs[tilenume] - artfilplc, SEEK_CUR);
        faketimerhandler();
    }

    {
        int64_t _t0 = esp_timer_get_time();

        if (!do_scale) {
            kread(artfil, waloff[tilenume], tileFilesize);
        } else {
            kread(artfil, tmp_buf, tileFilesize);
            uint8_t *dst = waloff[tilenume];
            for (int32_t x = 0; x < new_w; x++) {
                int32_t sx = (x * orig_w) / new_w;
                for (int32_t y = 0; y < new_h; y++) {
                    int32_t sy = (y * orig_h) / new_h;
                    dst[x * new_h + y] = tmp_buf[sx * orig_h + sy];
                }
            }
            heap_caps_free(tmp_buf);

            {
                int32_t pw = 0, ph = 0, w = new_w, h = new_h;
                while (w > 1) {
                    pw++;
                    w >>= 1;
                }
                while (h > 1) {
                    ph++;
                    h >>= 1;
                }
                picsiz[tilenume] = (uint8_t)(pw | (ph << 4));
            }
        }

        diag_tile_us += esp_timer_get_time() - _t0;
        diag_tile_loads += 1;
        diag_tile_bytes += tileFilesize;
    }
    faketimerhandler();
    artfilplc = tilefileoffs[tilenume] + tileFilesize;
}

void loadtile(short tilenume)
{
#ifdef DUKE3D_TILE_ASYNC_SPIKE
    if (g_tile_sd_mutex_ready) {
        if (xSemaphoreTakeRecursive(g_tile_sd_mutex, portMAX_DELAY) != pdTRUE)
            return;
    }
#endif
    loadtile_body(tilenume);
#ifdef DUKE3D_TILE_ASYNC_SPIKE
    if (g_tile_sd_mutex_ready)
        xSemaphoreGiveRecursive(g_tile_sd_mutex);
#endif
}

#ifdef DUKE3D_TILE_ASYNC_SPIKE
static int tile_async_try_schedule(short tilenume)
{
    if ((uint32_t)tilenume >= (uint32_t)MAXTILES)
        return 0;
    if (!g_tile_sd_mutex_ready || g_tile_async_q == NULL)
        return 0;
    if (!tile_async_spike_runtime_enabled)
        return 0;

    TileCacheHit hit;
    if (tilecache_lookup(tilenume, &hit))
        return 0;

    int32_t orig_w = tiles[tilenume].dim.width;
    int32_t orig_h = tiles[tilenume].dim.height;
    int32_t tileFilesize = orig_w * orig_h;
    if (tileFilesize <= 0)
        return 0;

    int32_t new_w = orig_w, new_h = orig_h;
    while (new_w > MAX_TILE_DIM) new_w >>= 1;
    while (new_h > MAX_TILE_DIM) new_h >>= 1;
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;

    int32_t do_scale = (new_w != orig_w || new_h != orig_h);
    uint8_t *tmp_probe = NULL;
    if (do_scale) {
        tmp_probe = (uint8_t *)heap_caps_malloc(tileFilesize,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp_probe)
            do_scale = 0;
        else
            heap_caps_free(tmp_probe);
    }
    int32_t cacheBytes = do_scale ? (new_w * new_h) : tileFilesize;

    /* allocache() eviction: cache.c treats *lock >= 200 as non-evictable and will
     * report "CACHE SPACE ALL LOCKED UP!" if the 256KB arena is full of them.
     * Use 199 like sync loadtile — placeholders participate in normal LRU. */
    if (xSemaphoreTakeRecursive(g_tile_sd_mutex, portMAX_DELAY) != pdTRUE)
        return 0;
    if (waloff[tilenume] != NULL) {
        xSemaphoreGiveRecursive(g_tile_sd_mutex);
        return 0;
    }

    tiles[tilenume].lock = 199;
    allocache(&waloff[tilenume], cacheBytes, (uint8_t *)&tiles[tilenume].lock);
    if (waloff[tilenume] == NULL) {
        tiles[tilenume].lock = 0;
        xSemaphoreGiveRecursive(g_tile_sd_mutex);
        return 0;
    }
    memset(waloff[tilenume], 0, (size_t)cacheBytes);

    if (do_scale) {
        int32_t pw = 0, ph = 0, w = new_w, h = new_h;
        while (w > 1) {
            pw++;
            w >>= 1;
        }
        while (h > 1) {
            ph++;
            h >>= 1;
        }
        picsiz[tilenume] = (uint8_t)(pw | (ph << 4));
    }

    /* Do not xQueueSend while holding the mutex: a full queue would block forever
     * while the worker waits on the same mutex. */
    xSemaphoreGiveRecursive(g_tile_sd_mutex);

    if (xQueueSend(g_tile_async_q, &tilenume, portMAX_DELAY) != pdTRUE) {
        tiles[tilenume].lock = 0;
        waloff[tilenume] = NULL;
        return 0;
    }
    return 1;
}

static void tile_async_worker_main(void *arg)
{
    (void)arg;
    short tilenume;
    for (;;) {
        if (xQueueReceive(g_tile_async_q, &tilenume, portMAX_DELAY) != pdTRUE)
            continue;
        if (!g_tile_sd_mutex_ready)
            continue;
        if (xSemaphoreTakeRecursive(g_tile_sd_mutex, portMAX_DELAY) != pdTRUE)
            continue;
        /* Slot may have been evicted (waloff cleared); skip quietly. */
        if (waloff[tilenume] != NULL)
            loadtile_grp_read_into_slot(tilenume);
        xSemaphoreGiveRecursive(g_tile_sd_mutex);
    }
}

void tile_async_spike_init(void)
{
    if (g_tile_sd_mutex_ready)
        return;
    g_tile_sd_mutex = xSemaphoreCreateRecursiveMutex();
    if (g_tile_sd_mutex == NULL) {
        printf("[tile_async_spike] mutex create failed\n");
        return;
    }
    g_tile_async_q = xQueueCreate(TILE_ASYNC_QUEUE_LEN, sizeof(short));
    if (g_tile_async_q == NULL) {
        vSemaphoreDelete(g_tile_sd_mutex);
        g_tile_sd_mutex = NULL;
        printf("[tile_async_spike] queue create failed\n");
        return;
    }
    g_tile_sd_mutex_ready = 1;
    if (xTaskCreatePinnedToCore(tile_async_worker_main, "tile_async", 4096, NULL, 3, NULL, 0) !=
        pdPASS) {
        printf("[tile_async_spike] worker task create failed\n");
        g_tile_sd_mutex_ready = 0;
        vQueueDelete(g_tile_async_q);
        g_tile_async_q = NULL;
        vSemaphoreDelete(g_tile_sd_mutex);
        g_tile_sd_mutex = NULL;
        return;
    }
    printf("[tile_async_spike] worker on core0, queue depth %d (placeholder=pal 0)\n",
           TILE_ASYNC_QUEUE_LEN);
}

#endif /* DUKE3D_TILE_ASYNC_SPIKE */


uint8_t* allocatepermanenttile(short tilenume, int32_t width, int32_t height)
{
    int32_t j;
    uint32_t tileDataSize;
    
    //Check dimensions are correct.
    if ((width <= 0) || (height <= 0) || ((uint32_t)tilenume >= (uint32_t)MAXTILES))
        return(0);
    
    tileDataSize = width * height;
    
    tiles[tilenume].lock = 255;
    allocache(&waloff[tilenume],tileDataSize,(uint8_t  *) &tiles[tilenume].lock);
    
    tiles[tilenume].dim.width = width;
    tiles[tilenume].dim.height = height;
    tiles[tilenume].animFlags = 0;
    
    j = 15;
    while ((j > 1) && (pow2long[j] > width))
        j--;
    picsiz[tilenume] = ((uint8_t )j);
    
    j = 15;
    while ((j > 1) && (pow2long[j] > height))
        j--;
    picsiz[tilenume] += ((uint8_t )(j<<4));
    
    return(waloff[tilenume]);
}



int loadpics(char  *filename, char * gamedir)

{
    int32_t offscount, localtilestart, localtileend, dasiz;
    short fil, i, j, k;
    
    
    strcpy(artfilename,filename);
    
    for(i=0; i<MAXTILES; i++)
    {
        tiles[i].dim.width = 0;
        tiles[i].dim.height = 0;
        tiles[i].animFlags = 0L;
    }
    
    artsize = 0L;
    
    numtilefiles = 0;
    do
    {
        k = numtilefiles;
        
        artfilename[7] = (k%10)+48;
        artfilename[6] = ((k/10)%10)+48;
        artfilename[5] = ((k/100)%10)+48;
        
        
        
        if ((fil = TCkopen4load(artfilename,0)) != -1)
        {
            kread32(fil,&artversion);
            if (artversion != 1) return(-1);
            
            kread32(fil,&numTiles);
            kread32(fil,&localtilestart);
            kread32(fil,&localtileend);
            
            /*kread(fil,&tilesizx[localtilestart],(localtileend-localtilestart+1)<<1);*/
            for (i = localtilestart; i <= localtileend; i++)
                kread16(fil,&tiles[i].dim.width);
            
            /*kread(fil,&tilesizy[localtilestart],(localtileend-localtilestart+1)<<1);*/
            for (i = localtilestart; i <= localtileend; i++)
                kread16(fil,&tiles[i].dim.height);
            
            /*kread(fil,&picanm[localtilestart],(localtileend-localtilestart+1)<<2);*/
            for (i = localtilestart; i <= localtileend; i++)
                kread32(fil,&tiles[i].animFlags);
            
            offscount = 4+4+4+4+((localtileend-localtilestart+1)<<3);
            for(i=localtilestart; i<=localtileend; i++)
            {
                tilefilenum[i] = k;
                tilefileoffs[i] = offscount;
                dasiz = tiles[i].dim.width*tiles[i].dim.height;
                offscount += dasiz;
                artsize += ((dasiz+15)&0xfffffff0);
            }
            kclose(fil);
            
            numtilefiles++;
            
        }
    }
    while (k != numtilefiles);
    
    printf("Art files loaded\n");
    
    clearbuf(gotpic,(MAXTILES+31)>>5,0L);

    // Allocate art cache from PSRAM — internal DRAM is fragmented after WiFi/SD/HUB75.
    // Cap at 256KB: sounds now live in a separate PSRAM heap pool (loadsound/clearsoundlocks
    // in sounds.c use heap_caps_malloc). Leaving ~200KB+ PSRAM heap free for sound buffers.
    cachesize = 256 * 1024;
    while ((pic = (uint8_t*)heap_caps_malloc(cachesize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) == NULL)
    {
        cachesize -= 64 * 1024;
        if (cachesize < 65536) return(-1);
    }
    initcache(pic,cachesize);
    
    for(i=0; i<MAXTILES; i++)
    {
        j = 15;
        while ((j > 1) && (pow2long[j] > tiles[i].dim.width)) 
            j--;
        
        picsiz[i] = ((uint8_t )j);
        j = 15;
        
        while ((j > 1) && (pow2long[j] > tiles[i].dim.height)) 
            j--;
        
        picsiz[i] += ((uint8_t )(j<<4));
    }
    
    artfil = -1;
    artfilnum = -1;
    artfilplc = 0L;
    
    return(0);
}


void TILE_MakeAvailable(short picID)
{
    if (waloff[picID] == NULL) {
#ifdef DUKE3D_TILE_ASYNC_SPIKE
        if (g_tile_sd_mutex_ready && tile_async_spike_runtime_enabled) {
            if (tile_async_try_schedule(picID))
                return;
        }
#endif
        loadtile(picID);
    }
}

void copytilepiece(int32_t tilenume1, int32_t sx1, int32_t sy1, int32_t xsiz, int32_t ysiz,
                   int32_t tilenume2, int32_t sx2, int32_t sy2)
{
    uint8_t  *ptr1, *ptr2, dat;
    int32_t xsiz1, ysiz1, xsiz2, ysiz2, i, j, x1, y1, x2, y2;
    
    xsiz1 = tiles[tilenume1].dim.width;
    ysiz1 = tiles[tilenume1].dim.height;
    
    xsiz2 = tiles[tilenume2].dim.width;
    ysiz2 = tiles[tilenume2].dim.height;
    
    
    if ((xsiz1 > 0) && (ysiz1 > 0) && (xsiz2 > 0) && (ysiz2 > 0))
    {
        TILE_MakeAvailable(tilenume1);
        TILE_MakeAvailable(tilenume2);
        
        x1 = sx1;
        for(i=0; i<xsiz; i++)
        {
            y1 = sy1;
            for(j=0; j<ysiz; j++)
            {
                x2 = sx2+i;
                y2 = sy2+j;
                if ((x2 >= 0) && (y2 >= 0) && (x2 < xsiz2) && (y2 < ysiz2))
                {
                    ptr1 = waloff[tilenume1] + x1*ysiz1 + y1;
                    ptr2 = waloff[tilenume2] + x2*ysiz2 + y2;
                    dat = *ptr1;
                    
                    
                    if (dat != 255)
                        *ptr2 = *ptr1;
                }
                
                y1++;
                if (y1 >= ysiz1) y1 = 0;
            }
            x1++;
            if (x1 >= xsiz1) x1 = 0;
        }
    }
}



/*
 FCS:   If a texture is animated, this will return the offset to add to tilenum
 in order to retrieve the texture to display.
 */
int animateoffs(int16_t tilenum)
{
    int32_t i, k, offs;
    
    offs = 0;
    
    i = (totalclocklock>>((tiles[tilenum].animFlags>>24)&15));
    
    if ((tiles[tilenum].animFlags&63) > 0){
        switch(tiles[tilenum].animFlags&192)
        {
            case 64:
                k = (i%((tiles[tilenum].animFlags&63)<<1));
                if (k < (tiles[tilenum].animFlags&63))
                    offs = k;
                else
                    offs = (((tiles[tilenum].animFlags&63)<<1)-k);
                break;
            case 128:
                offs = (i%((tiles[tilenum].animFlags&63)+1));
                break;
            case 192:
                offs = -(i%((tiles[tilenum].animFlags&63)+1));
        }
    }
    
    return(offs);
}