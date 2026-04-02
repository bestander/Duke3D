//-------------------------------------------------------------------------
/*
Copyright (C) 1996, 2003 - 3D Realms Entertainment

This file is part of Duke Nukem 3D version 1.5 - Atomic Edition

Duke Nukem 3D is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
aint32_t with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

Original Source: 1996 - Todd Replogle
Prepared for public release: 03/21/2003 - Charlie Wiederhold, 3D Realms
*/
//-------------------------------------------------------------------------

#if PLATFORM_DOS
#include <conio.h>
#endif

#ifndef PLATFORM_DOS
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "../audiolib/mv_stream.h"
#endif /* PLATFORM_DOS (early includes) */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "types.h"
#include "util_lib.h"
#include "duke3d.h"
#include "global.h"
#include "filesystem.h"

#ifndef PLATFORM_DOS
#define GRP_PATH "/sdcard/duke3d/DUKE3D.GRP"
static const char *SND_TAG = "sounds";

/* Per-sound streaming index built once at startup by SoundIndexGRP().
 * sound_grp_pcm_offset[n] = absolute byte offset of raw PCM in GRP file,
 * or -1 if the sound is not indexed (absent, looped, or parse error).
 * sound_pcm_rate_table[n] = source sample rate in Hz. */
EXT_RAM_ATTR static int32_t  sound_grp_pcm_offset[NUM_SOUNDS];
EXT_RAM_ATTR static uint32_t sound_pcm_rate_table[NUM_SOUNDS];

int Sound_IsStreamIndexed(uint16_t num)
{
    if (num >= NUM_SOUNDS) return 0;
    return sound_grp_pcm_offset[num] >= 0;
}

/* Parse a VOC file header to locate the raw PCM block.
 * hdr must contain at least the first 128 bytes of the file. */
static int parse_voc_header(const uint8_t *hdr,
                             int32_t *pcm_start, int32_t *pcm_len,
                             uint32_t *rate)
{
    /* VOC header layout:
     *   bytes  0-18: "Creative Voice File" (19 bytes)
     *   byte    19:  0x1A (EOF marker)
     *   bytes 20-21: uint16 LE offset to first data block (usually 26)
     *   bytes 22-23: version
     *   bytes 24-25: version XOR check */
    uint16_t data_off = (uint16_t)(hdr[20] | ((unsigned)hdr[21] << 8));
    if (data_off < 26 || data_off + 6 > 128) return 0;

    /* Block header at data_off: type(1) + size(3) + sr_div(1) + codec(1) */
    if (hdr[data_off] != 1) return 0;  /* must be Sound Data block */
    uint32_t block_size = (uint32_t)hdr[data_off+1]
                        | ((uint32_t)hdr[data_off+2] << 8)
                        | ((uint32_t)hdr[data_off+3] << 16);
    if (block_size < 2) return 0;
    uint8_t sr_div = hdr[data_off + 4];
    /* codec 0 = 8-bit unsigned PCM; accept only uncompressed */
    if (hdr[data_off + 5] != 0) return 0;

    *pcm_start = (int32_t)data_off + 6;
    *pcm_len   = (int32_t)(block_size - 2);
    *rate      = 1000000u / (256u - (unsigned)sr_div);
    return 1;
}

/* Parse RIFF WAVE: require PCM fmt + data chunk in first 128 bytes.
 * Streaming uses MV_PlayStream with bits=8 and mono mix — reject anything else
 * so we do not index 16-bit/stereo WAV as wrong-length 8-bit (silent/garbled). */
static int parse_wav_header(const uint8_t *hdr,
                             int32_t *pcm_start, int32_t *pcm_len,
                             uint32_t *rate)
{
    int      fmt_ok = 0;
    uint32_t fmt_rate = 11025;

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return 0;

    int pos = 12;
    while (pos + 8 <= 128) {
        uint32_t sz = (uint32_t)hdr[pos + 4] | ((uint32_t)hdr[pos + 5] << 8)
                    | ((uint32_t)hdr[pos + 6] << 16) | ((uint32_t)hdr[pos + 7] << 24);
        int chunk_body = (int)sz;
        int skip       = 8 + chunk_body + (chunk_body & 1); /* word-align */

        if (memcmp(hdr + pos, "fmt ", 4) == 0) {
            int d0 = pos + 8;
            if (d0 + 16 > 128 || sz < 16)
                return 0;
            uint16_t audio_fmt = (uint16_t)hdr[d0] | ((uint16_t)hdr[d0 + 1] << 8);
            uint16_t channels  = (uint16_t)hdr[d0 + 2] | ((uint16_t)hdr[d0 + 3] << 8);
            fmt_rate = (uint32_t)hdr[d0 + 4] | ((uint32_t)hdr[d0 + 5] << 8)
                     | ((uint32_t)hdr[d0 + 6] << 16) | ((uint32_t)hdr[d0 + 7] << 24);
            uint16_t bps = (uint16_t)hdr[d0 + 14] | ((uint16_t)hdr[d0 + 15] << 8);
            if (audio_fmt != 1u || channels != 1u || bps != 8u)
                return 0;
            fmt_ok = 1;
        } else if (memcmp(hdr + pos, "data", 4) == 0) {
            if (!fmt_ok)
                return 0;
            *pcm_len   = (int32_t)sz;
            *pcm_start = pos + 8;
            *rate      = fmt_rate;
            return (*pcm_len > 0) ? 1 : 0;
        }

        if (skip < 8 || pos + skip > 128)
            break;
        pos += skip;
    }
    return 0;
}

/* Build the streaming index: scan the GRP directory once at startup.
 * Fills sound_grp_pcm_offset[] and sound_pcm_rate_table[].
 * Called from SoundStartup() after FX_Init succeeds. */
static void SoundIndexGRP(void)
{
    memset(sound_grp_pcm_offset, 0xFF, sizeof(sound_grp_pcm_offset)); /* -1 */

    FILE *f = fopen(GRP_PATH, "rb");
    if (!f) { ESP_LOGE(SND_TAG, "SoundIndexGRP: cannot open %s", GRP_PATH); return; }

    char magic[12];
    uint32_t num_files;
    if (fread(magic, 1, 12, f) != 12 || fread(&num_files, 4, 1, f) != 1
        || memcmp(magic, "KenSilverman", 12) != 0 || num_files == 0) {
        ESP_LOGE(SND_TAG, "SoundIndexGRP: bad GRP header");
        fclose(f); return;
    }

    /* Allocate directory buffer temporarily */
    size_t dir_bytes = (size_t)num_files * 16;
    uint8_t *dir = (uint8_t *)malloc(dir_bytes);
    if (!dir) { ESP_LOGE(SND_TAG, "SoundIndexGRP: OOM for dir"); fclose(f); return; }

    if (fread(dir, 1, dir_bytes, f) != dir_bytes) {
        ESP_LOGE(SND_TAG, "SoundIndexGRP: short directory read");
        free(dir); fclose(f); return;
    }

    uint32_t data_base = 16 + (uint32_t)dir_bytes;  /* offset where file data starts */
    uint32_t file_off  = 0;

    for (uint32_t i = 0; i < num_files; i++) {
        const char *grp_name = (const char *)(dir + i * 16);
        uint32_t    grp_size = (uint32_t)dir[i*16+12] | ((uint32_t)dir[i*16+13] << 8)
                             | ((uint32_t)dir[i*16+14] << 16) | ((uint32_t)dir[i*16+15] << 24);
        char name_buf[13];
        memcpy(name_buf, grp_name, 12);
        name_buf[12] = '\0';

        for (int j = 0; j < NUM_SOUNDS; j++) {
            if (sounds[j][0] == '\0') continue;
            if (strcasecmp(name_buf, sounds[j]) == 0) {
                sound_grp_pcm_offset[j] = (int32_t)(data_base + file_off);
                soundsiz[j] = (int32_t)grp_size;
                break;
            }
        }
        file_off += grp_size;
    }
    free(dir);

    /* Parse headers to convert file offsets to PCM offsets */
    uint8_t hdr[128];
    int indexed = 0;
    for (int j = 0; j < NUM_SOUNDS; j++) {
        if (sound_grp_pcm_offset[j] < 0) continue;
        fseek(f, (long)sound_grp_pcm_offset[j], SEEK_SET);
        if ((int)fread(hdr, 1, sizeof(hdr), f) < 32) {
            sound_grp_pcm_offset[j] = -1; continue;
        }
        int32_t  pcm_start = 0, pcm_len = 0;
        uint32_t rate = 11025;
        int ok;
        if (hdr[0] == 'C')
            ok = parse_voc_header(hdr, &pcm_start, &pcm_len, &rate);
        else if (hdr[0] == 'R')
            ok = parse_wav_header(hdr, &pcm_start, &pcm_len, &rate);
        else
            ok = 0;

        if (!ok || pcm_len <= 0) { sound_grp_pcm_offset[j] = -1; continue; }

        sound_grp_pcm_offset[j] += pcm_start;   /* absolute GRP offset of PCM */
        soundsiz[j]               = pcm_len;
        sound_pcm_rate_table[j]   = rate;
        indexed++;
    }
    fclose(f);

    ESP_LOGD(SND_TAG, "SoundIndexGRP: %d/%d sounds indexed for streaming", indexed, NUM_SOUNDS);

    /* Open the persistent GRP fd used by the audio task for ongoing streaming */
    MV_OpenGRPStream(GRP_PATH);
}

/* Play a non-looped sound via SD streaming.
 * Opens GRP briefly for the initial chunk (up to MV_STREAM_HALF) so the first
 * mix period has data immediately (no 23 ms silence). */
static int play_sound_stream(int32_t pcm_abs_offset, int32_t pcm_len,
                              uint32_t rate, int pitch,
                              int angle, int distance,
                              int priority, int32_t callbackval)
{
    uint8_t  init_buf[4096];  /* must be >= MV_STREAM_HALF in multivoc.c */
    int32_t  init_len = 0;
    {
        size_t want = sizeof(init_buf);
        if (pcm_len > 0 && (int32_t)want > pcm_len)
            want = (size_t)pcm_len;
        int n = MV_GrpStreamReadAt((int64_t)pcm_abs_offset, init_buf, want);
        if (n > 0)
            init_len = n;
        else if (n < 0)
            ESP_LOGW(SND_TAG, "play_sound_stream: MV_GrpStreamReadAt failed for init read");
    }
    return MV_PlayStream3D(pcm_abs_offset, pcm_len, (unsigned)rate,
                           init_buf, init_len,
                           pitch, angle, distance, priority,
                           (unsigned long)callbackval);
}
#endif /* PLATFORM_DOS */


#define LOUDESTVOLUME 150

int32_t backflag,numenvsnds;

/*
===================
=
= SoundStartup
=
===================
*/

void SoundStartup( void )
   {
   int32 status;

   // if they chose None lets return
   if (FXDevice == NumSoundCards) return;

   // Do special Sound Blaster, AWE32 stuff
   if (
         ( FXDevice == SoundBlaster ) ||
         ( FXDevice == Awe32 )
      )
      {
      int MaxVoices;
      int MaxBits;
      int MaxChannels;

      status = FX_SetupSoundBlaster
                  (
                  BlasterConfig, (int *)&MaxVoices, (int *)&MaxBits, (int *)&MaxChannels
                  );
      }
   else
      {
      status = FX_Ok;
      }

   if ( status == FX_Ok )
      {
      if ( eightytwofifty && numplayers > 1)
         {
         status = FX_Init( FXDevice, min( NumVoices,4 ), 1, 8, 8000 );
         }
      else
         {
         status = FX_Init( FXDevice, NumVoices, NumChannels, NumBits, MixRate );
         }
      if ( status == FX_Ok )
         {

         FX_SetVolume( FXVolume );
         if (ReverseStereo == 1)
            {
            FX_SetReverseStereo(!FX_GetReverseStereo());
            }
         }
      }
   if ( status != FX_Ok )
      {
      Error(EXIT_FAILURE, FX_ErrorString( FX_Error ));
      }

#ifndef PLATFORM_DOS
   /* Build streaming index: scan GRP directory, parse VOC/WAV headers.
    * After this call, sound_grp_pcm_offset[n] >= 0 for all indexable sounds.
    * Also opens the persistent GRP fd used by the audio task. */
   SoundIndexGRP();
#endif

   status = FX_SetCallBack( TestCallBack );

   if ( status != FX_Ok )
      {
      Error(EXIT_FAILURE, FX_ErrorString( FX_Error ));
      }
   }

/*
===================
=
= SoundShutdown
=
===================
*/

void SoundShutdown( void )
   {
   int32 status;

   // if they chose None lets return
   if (FXDevice == NumSoundCards)
      return;

   status = FX_Shutdown();
   if ( status != FX_Ok )
      {
      Error(EXIT_FAILURE, FX_ErrorString( FX_Error ));
      }
   }

/*
===================
=
= MusicStartup
=
===================
*/

void MusicStartup( void )
   {
   int32 status;

   // if they chose None lets return
   if ((MusicDevice == NumSoundCards) || (eightytwofifty && numplayers > 1) )
      return;

   // satisfy AWE32 and WAVEBLASTER stuff
   BlasterConfig.Midi = MidiPort;

   // Do special Sound Blaster, AWE32 stuff
   if (
         ( FXDevice == SoundBlaster ) ||
         ( FXDevice == Awe32 )
      )
      {
      int MaxVoices;
      int MaxBits;
      int MaxChannels;

      FX_SetupSoundBlaster
                  (
                  BlasterConfig, (int *)&MaxVoices, (int *)&MaxBits, (int *)&MaxChannels
                  );
      }
   status = MUSIC_Init( MusicDevice, MidiPort );

   if ( status == MUSIC_Ok )
      {
      MUSIC_SetVolume( MusicVolume );
      }
   else
   {
      SoundShutdown();
      uninittimer();
      uninitengine();
      CONTROL_Shutdown();
      CONFIG_WriteSetup();
      KB_Shutdown();
      uninitgroupfile();
      unlink("duke3d.tmp");
      Error(EXIT_FAILURE, "Couldn't find selected sound card, or, error w/ sound card itself\n");
   }
}

/*
===================
=
= MusicShutdown
=
===================
*/

void MusicShutdown( void )
   {
   int32 status;

   // if they chose None lets return
   if ((MusicDevice == NumSoundCards) || (eightytwofifty && numplayers > 1) )
      return;

   status = MUSIC_Shutdown();
   if ( status != MUSIC_Ok )
      {
      //Error( MUSIC_ErrorString( MUSIC_ErrorCode ));
      }
   }


int USRHOOKS_GetMem(void  **ptr, uint32_t size )
{
   *ptr = malloc(size);

   if (*ptr == NULL)
      return(USRHOOKS_Error);

   return( USRHOOKS_Ok);

}

int USRHOOKS_FreeMem(void  *ptr)
{
   free(ptr);
   return( USRHOOKS_Ok);
}

uint8_t  menunum=0;

void intomenusounds(void)
{
    static const short menusnds[] =
    {
        LASERTRIP_EXPLODE,
        DUKE_GRUNT,
        DUKE_LAND_HURT,
        CHAINGUN_FIRE,
        SQUISHED,
        KICK_HIT,
        PISTOL_RICOCHET,
        PISTOL_BODYHIT,
        PISTOL_FIRE,
        SHOTGUN_FIRE,
        BOS1_WALK,
        RPG_EXPLODE,
        PIPEBOMB_BOUNCE,
        PIPEBOMB_EXPLODE,
        NITEVISION_ONOFF,
        RPG_SHOOT,
        SELECT_WEAPON
    };
    sound(menusnds[(int)menunum++]);
    menunum %= 17;
}

void playmusic(char  *fn)
{
    if(MusicToggle == 0) return;
    if(MusicDevice == NumSoundCards) return;

    // the SDL_mixer version does more or less this same thing.  --ryan.
    PlayMusic(fn);
}

uint8_t  loadsound(uint16_t num)
{
    int32_t   fp, l;

    if(num >= NUM_SOUNDS || SoundToggle == 0) return 0;
    if (FXDevice == NumSoundCards) return 0;

#ifndef PLATFORM_DOS
    /* One-shot stream path keeps ptr==0; looped FX need full VOC/WAV in RAM (loop offset at ptr+0x14). */
    if (Sound_IsStreamIndexed(num) && (soundm[num] & 1) == 0)
        return 1;
#endif

    fp = TCkopen4load(sounds[num],0);
    if(fp == -1)
    {
        sprintf(&fta_quotes[113][0],"Sound %s(#%d) not found.",sounds[num],num);
        FTA(113,&ps[myconnectindex],1);
        return 0;
    }

    l = kfilelength( fp );
    soundsiz[num] = l;

    /* Allocate from PSRAM heap — keeps sound data out of the tile cache. */
    Sound[num].ptr = (uint8_t *)heap_caps_malloc((size_t)l,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!Sound[num].ptr)
    {
        kclose(fp);
        return 0;   /* OOM — sound skipped silently */
    }
    Sound[num].lock = 1;  /* nominal; no longer a tile-cache lock */

    kread( fp, Sound[num].ptr , l);
    kclose( fp );
    return 1;
}

int xyzsound(short num,short i,int32_t x,int32_t y,int32_t z)
{
    int32_t sndist, cx, cy, cz, j,k;
    short pitche,pitchs,cs;
    int voice, sndang, ca, pitch;
	
    if( num >= NUM_SOUNDS ||
        FXDevice == NumSoundCards ||
        ( (soundm[num]&8) && ud.lockout ) ||
        SoundToggle == 0 ||
        Sound[num].num > 3 ||
        FX_VoiceAvailable(soundpr[num]) == 0 ||
        (ps[myconnectindex].timebeforeexit > 0 && ps[myconnectindex].timebeforeexit <= 26*3) ||
        ps[myconnectindex].gm&MODE_MENU) return -1;

    if( soundm[num]&128 )
    {
        sound(num);
        return 0;
    }

    if( soundm[num]&4 )
    {
		// FIX_00041: Toggle to hear the opponent sound in DM (like it used to be in v1.3d)
		if(VoiceToggle==0 || (ud.multimode > 1 && PN == APLAYER && sprite[i].yvel != screenpeek && /*ud.coop!=1 &&*/ !OpponentSoundToggle) ) return -1; //xduke : 1.3d Style: makes opponent sound in DM as in COOP

        for(j=0;j<NUM_SOUNDS;j++)
          for(k=0;k<Sound[j].num;k++)
            if( (Sound[j].num > 0) && (soundm[j]&4) )
              return -1;
    }

    cx = ps[screenpeek].oposx;
    cy = ps[screenpeek].oposy;
    cz = ps[screenpeek].oposz;
    cs = ps[screenpeek].cursectnum;
    ca = ps[screenpeek].ang+ps[screenpeek].look_ang;

    sndist = FindDistance3D((cx-x),(cy-y),(cz-z)>>4);

    if( i >= 0 && (soundm[num]&16) == 0 && PN == MUSICANDSFX && SLT < 999 && (sector[SECT].lotag&0xff) < 9 )
        sndist = divscale14(sndist,(SHT+1));

    pitchs = soundps[num];
    pitche = soundpe[num];
    cx = klabs(pitche-pitchs);

    if(cx)
    {
        if( pitchs < pitche )
             pitch = pitchs + ( rand()%cx );
        else pitch = pitche + ( rand()%cx );
    }
    else pitch = pitchs;

    sndist += soundvo[num];
    if(sndist < 0) sndist = 0;
    if( sndist && PN != MUSICANDSFX && !cansee(cx,cy,cz-(24<<8),cs,SX,SY,SZ-(24<<8),SECT) )
        sndist += sndist>>5;

    switch(num)
    {
        case PIPEBOMB_EXPLODE:
        case LASERTRIP_EXPLODE:
        case RPG_EXPLODE:
            if(sndist > (6144) )
                sndist = 6144;
            if(sector[ps[screenpeek].cursectnum].lotag == 2)
                pitch -= 1024;
            break;
        default:
            if(sector[ps[screenpeek].cursectnum].lotag == 2 && (soundm[num]&4) == 0)
                pitch = -768;
            if( sndist > 31444 && PN != MUSICANDSFX)
                return -1;
            break;
    }


    if( Sound[num].num > 0 && PN != MUSICANDSFX )
    {
        if( SoundOwner[num][0].i == i ) stopsound(num);
        else if( Sound[num].num > 1 ) stopsound(num);
        else if( badguy(&sprite[i]) && sprite[i].extra <= 0 ) stopsound(num);
    }

    if( PN == APLAYER && sprite[i].yvel == screenpeek )
    {
        sndang = 0;
        sndist = 0;
    }
    else
    {
        sndang = 2048 + ca - getangle(cx-x,cy-y);
        sndang &= 2047;
    }

    if( soundm[num]&16 ) sndist = 0;

    if(sndist < ((255-LOUDESTVOLUME)<<6) )
        sndist = ((255-LOUDESTVOLUME)<<6);

    if( soundm[num]&1 )
    {
        /* Looped ambient sound — must be fully loaded in PSRAM (loop pointers
         * into the buffer; streaming cannot handle these). */
        uint16_t start;

        if(Sound[num].num > 0) return -1;

        if(Sound[num].ptr == 0) { if( loadsound(num) == 0 ) return 0; }
        else
        {
           if (Sound[num].lock < 200) Sound[num].lock = 200;
           else Sound[num].lock++;
        }

        start = *(uint16_t *)(Sound[num].ptr + 0x14);

        if(*Sound[num].ptr == 'C')
            voice = FX_PlayLoopedVOC( Sound[num].ptr, start, start + soundsiz[num],
                    pitch,sndist>>6,sndist>>6,0,soundpr[num],num);
        else
            voice = FX_PlayLoopedWAV( Sound[num].ptr, start, start + soundsiz[num],
                    pitch,sndist>>6,sndist>>6,0,soundpr[num],num);
    }
#ifndef PLATFORM_DOS
    else if( sound_grp_pcm_offset[num] >= 0 )
    {
        /* Non-looped sound with a valid streaming index: stream from GRP.
         * Drop any allocache buffer getsound() may have set — it was never
         * used and clearsoundlocks() must not heap_caps_free an allocache ptr. */
        Sound[num].ptr = 0;
        Sound[num].lock = (Sound[num].lock < 200) ? 200 : Sound[num].lock + 1;
        voice = play_sound_stream(sound_grp_pcm_offset[num], soundsiz[num],
                                  sound_pcm_rate_table[num],
                                  pitch, sndang>>6, sndist>>6,
                                  soundpr[num], num);
    }
#endif
    else
    {
        /* Fallback: load into PSRAM (streaming index unavailable). */
        if(Sound[num].ptr == 0) { if( loadsound(num) == 0 ) return 0; }
        else
        {
           if (Sound[num].lock < 200) Sound[num].lock = 200;
           else Sound[num].lock++;
        }
        if( *Sound[num].ptr == 'C')
            voice = FX_PlayVOC3D( Sound[ num ].ptr,pitch,sndang>>6,sndist>>6, soundpr[num], num );
        else voice = FX_PlayWAV3D( Sound[ num ].ptr,pitch,sndang>>6,sndist>>6, soundpr[num], num );
    }

    if ( voice > FX_Ok )
    {
        SoundOwner[num][Sound[num].num].i = i;
        SoundOwner[num][Sound[num].num].voice = voice;
        Sound[num].num++;
    }
    else Sound[num].lock--;
    return (voice);
}

void sound(short num)
{
    short pitch,pitche,pitchs,cx;
    int voice;
    int32_t start;

    if (FXDevice == NumSoundCards) return;
    if(SoundToggle==0) return;
    if(VoiceToggle==0 && (soundm[num]&4) ) return;
    if( (soundm[num]&8) && ud.lockout ) return;
    if(FX_VoiceAvailable(soundpr[num]) == 0) return;

    pitchs = soundps[num];
    pitche = soundpe[num];
    cx = klabs(pitche-pitchs);

    if(cx)
    {
        if( pitchs < pitche )
             pitch = pitchs + ( rand()%cx );
        else pitch = pitche + ( rand()%cx );
    }
    else pitch = pitchs;

    if( soundm[num]&1 )
    {
        /* Looped sound — must be fully loaded in PSRAM. */
        if(Sound[num].ptr == 0) { if( loadsound(num) == 0 ) return; }
        else
        {
           if (Sound[num].lock < 200) Sound[num].lock = 200;
           else Sound[num].lock++;
        }
        if(*Sound[num].ptr == 'C')
        {
            start = (int32_t)*(uint16_t *)(Sound[num].ptr + 0x14);
            voice = FX_PlayLoopedVOC( Sound[num].ptr, start, start + soundsiz[num],
                    pitch,LOUDESTVOLUME,LOUDESTVOLUME,LOUDESTVOLUME,soundpr[num],num);
        }
        else
        {
            start = (int32_t)*(uint16_t *)(Sound[num].ptr + 0x14);
            voice = FX_PlayLoopedWAV( Sound[num].ptr, start, start + soundsiz[num],
                    pitch,LOUDESTVOLUME,LOUDESTVOLUME,LOUDESTVOLUME,soundpr[num],num);
        }
    }
#ifndef PLATFORM_DOS
    else if( sound_grp_pcm_offset[num] >= 0 )
    {
        /* Non-looped sound: stream from GRP at full (loudest) volume.
         * Drop any allocache buffer getsound() may have set. */
        Sound[num].ptr = 0;
        Sound[num].lock = (Sound[num].lock < 200) ? 200 : Sound[num].lock + 1;
        voice = play_sound_stream(sound_grp_pcm_offset[num], soundsiz[num],
                                  sound_pcm_rate_table[num],
                                  pitch, 0, 255-LOUDESTVOLUME,
                                  soundpr[num], num);
    }
#endif
    else
    {
        /* Fallback: PSRAM load. */
        if(Sound[num].ptr == 0) { if( loadsound(num) == 0 ) return; }
        else
        {
           if (Sound[num].lock < 200) Sound[num].lock = 200;
           else Sound[num].lock++;
        }
        if(*Sound[num].ptr == 'C')
            voice = FX_PlayVOC3D( Sound[ num ].ptr, pitch,0,255-LOUDESTVOLUME,soundpr[num], num );
        else
            voice = FX_PlayWAV3D( Sound[ num ].ptr, pitch,0,255-LOUDESTVOLUME,soundpr[num], num );
    }

    if(voice > FX_Ok) return;
    Sound[num].lock--;
}

int spritesound(uint16_t num, short i)
{
    if(num >= NUM_SOUNDS) return -1;
    return xyzsound(num,i,SX,SY,SZ);
}

void stopsound(short num)
{
    if(Sound[num].num > 0)
    {
        FX_StopSound(SoundOwner[num][Sound[num].num-1].voice);
        testcallback(num);
    }
}

void stopenvsound(short num,short i)
{
    short j, k;

    if(Sound[num].num > 0)
    {
        k = Sound[num].num;
        for(j=0;j<k;j++)
           if(SoundOwner[num][j].i == i)
        {
            FX_StopSound(SoundOwner[num][j].voice);
            break;
        }
    }
}

void pan3dsound(void)
{
    int32_t sndist, sx, sy, sz, cx, cy, cz;
    short sndang,ca,j,k,i,cs;

    numenvsnds = 0;

    if(ud.camerasprite == -1)
    {
        cx = ps[screenpeek].oposx;
        cy = ps[screenpeek].oposy;
        cz = ps[screenpeek].oposz;
        cs = ps[screenpeek].cursectnum;
        ca = ps[screenpeek].ang+ps[screenpeek].look_ang;
    }
    else
    {
        cx = sprite[ud.camerasprite].x;
        cy = sprite[ud.camerasprite].y;
        cz = sprite[ud.camerasprite].z;
        cs = sprite[ud.camerasprite].sectnum;
        ca = sprite[ud.camerasprite].ang;
    }

    for(j=0;j<NUM_SOUNDS;j++) for(k=0;k<Sound[j].num;k++)
    {
        i = SoundOwner[j][k].i;

        sx = sprite[i].x;
        sy = sprite[i].y;
        sz = sprite[i].z;

        if( PN == APLAYER && sprite[i].yvel == screenpeek)
        {
            sndang = 0;
            sndist = 0;
        }
        else
        {
            sndang = 2048 + ca - getangle(cx-sx,cy-sy);
            sndang &= 2047;
            sndist = FindDistance3D((cx-sx),(cy-sy),(cz-sz)>>4);
            if( i >= 0 && (soundm[j]&16) == 0 && PN == MUSICANDSFX && SLT < 999 && (sector[SECT].lotag&0xff) < 9 )
                sndist = divscale14(sndist,(SHT+1));
        }

        sndist += soundvo[j];
        if(sndist < 0) sndist = 0;

        if( sndist && PN != MUSICANDSFX && !cansee(cx,cy,cz-(24<<8),cs,sx,sy,sz-(24<<8),SECT) )
            sndist += sndist>>5;

        if(PN == MUSICANDSFX && SLT < 999)
            numenvsnds++;

        switch(j)
        {
            case PIPEBOMB_EXPLODE:
            case LASERTRIP_EXPLODE:
            case RPG_EXPLODE:
                if(sndist > (6144)) sndist = (6144);
                break;
            default:
                if( sndist > 31444 && PN != MUSICANDSFX)
                {
                    stopsound(j);
                    continue;
                }
        }

        if(Sound[j].ptr == 0 && loadsound(j) == 0 ) continue;
        if( soundm[j]&16 ) sndist = 0;

        if(sndist < ((255-LOUDESTVOLUME)<<6) )
            sndist = ((255-LOUDESTVOLUME)<<6);

        FX_Pan3D(SoundOwner[j][k].voice,sndang>>6,sndist>>6);
    }
}

void TestCallBack(int32_t num)
{
    short tempi,tempj,tempk;

        if(num < 0)
        {
            if(lumplockbyte[-num] >= 200)
                lumplockbyte[-num]--;
            return;
        }

        tempk = Sound[num].num;

        if(tempk > 0)
        {
            if( (soundm[num]&16) == 0)
                for(tempj=0;tempj<tempk;tempj++)
            {
                tempi = SoundOwner[num][tempj].i;
                if(sprite[tempi].picnum == MUSICANDSFX && sector[sprite[tempi].sectnum].lotag < 3 && sprite[tempi].lotag < 999)
                {
                    hittype[tempi].temp_data[0] = 0;
                    if( (tempj + 1) < tempk )
                    {
                        SoundOwner[num][tempj].voice = SoundOwner[num][tempk-1].voice;
                        SoundOwner[num][tempj].i     = SoundOwner[num][tempk-1].i;
                    }
                    break;
                }
            }

            Sound[num].num--;
            SoundOwner[num][tempk-1].i = -1;
        }

        Sound[num].lock--;
}


// no idea if this is right. I added this function.  --ryan.
void testcallback(uint32_t num)
{
//    STUBBED("wtf?");
    TestCallBack(num);
}


void clearsoundlocks(void)
{
    int32_t i;

    for(i=0;i<NUM_SOUNDS;i++)
    {
        /* Free PSRAM-allocated sound buffers that are not currently playing.
         * num > 0 means at least one active voice is streaming from ptr — keep those.
         * Next play will reload from SD via loadsound() when ptr == 0. */
        if (Sound[i].ptr != 0 && Sound[i].num == 0)
        {
            heap_caps_free(Sound[i].ptr);
            Sound[i].ptr = 0;
        }
        Sound[i].lock = 199;
    }

    for(i=0;i<11;i++)
        lumplockbyte[i] = 199;
}

