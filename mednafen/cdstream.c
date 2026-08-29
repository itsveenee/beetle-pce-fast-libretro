/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <string.h>

#include "cdstream.h"

#ifdef AURORA_PS2_PCE_FAST
/* AURORA_CD_AUDIO_STREAM_V2_PCE_CACHE_20260829
 *
 * One cache for the active read stream, not one cache per CUE track.
 * This keeps memory bounded while turning sequential 2352-byte CD reads into
 * large mass:/ transfers. */
#define AURORA_PCE_STREAM_CACHE_BYTES (128 * 1024)

static uint8_t s_AuroraPceStreamCache[AURORA_PCE_STREAM_CACHE_BYTES]
   __attribute__((aligned(64)));
static cdstream *s_AuroraPceStreamCacheOwner;
static int64_t s_AuroraPceStreamCacheStart;
static uint64_t s_AuroraPceStreamCacheLength;

void cdstream_ps2_cache_forget(cdstream *s)
{
   if (s_AuroraPceStreamCacheOwner == s)
   {
      s_AuroraPceStreamCacheOwner = NULL;
      s_AuroraPceStreamCacheLength = 0;
   }
}

uint64_t cdstream_ps2_read_cached(cdstream *s, void *data, uint64_t count)
{
   uint8_t *out = (uint8_t *)data;
   uint64_t total = 0;

   if (!s || !s->fp || !data || count == 0)
      return 0;

   while (count > 0)
   {
      uint64_t available = 0;

      if (s_AuroraPceStreamCacheOwner == s &&
          s->pos >= s_AuroraPceStreamCacheStart &&
          (uint64_t)(s->pos - s_AuroraPceStreamCacheStart) <
             s_AuroraPceStreamCacheLength)
      {
         available = s_AuroraPceStreamCacheLength -
            (uint64_t)(s->pos - s_AuroraPceStreamCacheStart);
      }
      else
      {
         uint64_t want = AURORA_PCE_STREAM_CACHE_BYTES;
         int64_t got;

         if (s->pos < 0)
            break;

         if (s->size > 0)
         {
            if ((uint64_t)s->pos >= s->size)
               break;
            if (s->size - (uint64_t)s->pos < want)
               want = s->size - (uint64_t)s->pos;
         }

         filestream_seek(s->fp, s->pos, RETRO_VFS_SEEK_POSITION_START);
         got = filestream_read(s->fp, s_AuroraPceStreamCache, (int64_t)want);
         if (got <= 0)
         {
            s_AuroraPceStreamCacheOwner = NULL;
            s_AuroraPceStreamCacheLength = 0;
            break;
         }

         s_AuroraPceStreamCacheOwner = s;
         s_AuroraPceStreamCacheStart = s->pos;
         s_AuroraPceStreamCacheLength = (uint64_t)got;
         available = (uint64_t)got;
      }

      if (available > count)
         available = count;

      memcpy(out,
             s_AuroraPceStreamCache +
                (size_t)(s->pos - s_AuroraPceStreamCacheStart),
             (size_t)available);

      out += available;
      count -= available;
      total += available;
      s->pos += (int64_t)available;
   }

   return total;
}
#endif

bool cdstream_open(cdstream *out, const char *path)
{
   memset(out, 0, sizeof(*out));
   out->fp = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
#ifdef AURORA_PS2_PCE_FAST
   /* AURORA_CD_AUDIO_STREAM_V2_PCE_OPEN_20260829 */
   if (out->fp)
   {
      int64_t size = filestream_get_size(out->fp);
      out->size = size > 0 ? (uint64_t)size : 0;
      out->pos = 0;
      out->cacheable = true;
   }
#endif
   return out->fp != NULL;
}

bool cdstream_open_write(cdstream *out, const char *path)
{
   memset(out, 0, sizeof(*out));
   out->fp = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   return out->fp != NULL;
}

bool cdstream_open_memcached(cdstream *out, const char *path)
{
   void   *buf = NULL;
   int64_t len = 0;

   memset(out, 0, sizeof(*out));

   /* filestream_read_file returns non-zero on success and allocates
    * buf with malloc; we own it from this point on. */
   if (!filestream_read_file(path, &buf, &len) || !buf)
   {
      if (buf)
         free(buf);
      return false;
   }

   /* Defensive: a zero-length file is "open and empty", not failure.
    * The historical MemoryStream path treated an empty source as a
    * NULL buffer with size 0 and reported is_valid=false, which made
    * the caller treat zero-byte files as "could not load".  Preserve
    * that behaviour to avoid surprising any existing call site. */
   if (len <= 0)
   {
      free(buf);
      return false;
   }

   out->buf  = (uint8_t *)buf;
   out->size = (uint64_t)len;
   out->pos  = 0;
   return true;
}

int cdstream_get_line(cdstream *s, char *out, size_t cap)
{
   size_t  n       = 0;
   bool    got_any = false;
   uint8_t c;

   /* Memory-backed fast path: tight inline scan over s->buf. */
   if (s->buf)
   {
      if (s->pos < 0)
         return -1;

      while ((uint64_t)s->pos < s->size)
      {
         c        = s->buf[s->pos++];
         got_any  = true;

         if (c == '\r' || c == '\n' || c == 0)
         {
            if (cap > 0)
               out[n] = '\0';
            return c;
         }

         if (cap > 0 && n + 1 < cap)
            out[n++] = (char)c;
      }

      if (cap > 0)
         out[n] = '\0';
      return got_any ? 0 : -1;
   }

   /* File-backed fallback: byte-at-a-time over filestream_read.
    * Always NUL-terminate out.  cap == 0 means "no buffer"; still
    * drain the line and report line-end / EOF correctly. */
   if (!s->fp)
      return -1;

   for (;;)
   {
      if (cdstream_read(s, &c, 1) == 0)
      {
         if (cap > 0)
            out[n] = '\0';
         return got_any ? 0 : -1;
      }
      got_any = true;

      if (c == '\r' || c == '\n' || c == 0)
      {
         if (cap > 0)
            out[n] = '\0';
         return c;
      }

      /* Cap-1 to leave room for NUL. Once full, silently drop further
       * bytes until we hit the line-end. */
      if (cap > 0 && n + 1 < cap)
         out[n++] = (char)c;
   }
}

cdstream *cdstream_new(const char *path)
{
   cdstream *s = (cdstream *)malloc(sizeof(*s));
   if (!s)
      return NULL;
   if (!cdstream_open(s, path))
   {
      free(s);
      return NULL;
   }
   return s;
}

cdstream *cdstream_new_memcached(const char *path)
{
   cdstream *s = (cdstream *)malloc(sizeof(*s));
   if (!s)
      return NULL;
   if (!cdstream_open_memcached(s, path))
   {
      free(s);
      return NULL;
   }
   return s;
}
