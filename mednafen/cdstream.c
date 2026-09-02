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
/* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830
 * Keep the Beetle build independent of PS2SDK include paths.
 * Layout matches the EE kernel ABI; prototypes take opaque pointers. */
typedef struct AuroraPceEeSemaT
{
   int count, max_count, init_count, wait_threads;
   unsigned int attr, option;
} AuroraPceEeSemaT;

typedef struct AuroraPceEeThreadT
{
   int status;
   void *func;
   void *stack;
   int stack_size;
   void *gp_reg;
   int initial_priority;
   int current_priority;
   unsigned int attr;
   unsigned int option;
} AuroraPceEeThreadT;

typedef struct AuroraPceEeThreadStatusT
{
   int status;
   void *func;
   void *stack;
   int stack_size;
   void *gp_reg;
   int initial_priority;
   int current_priority;
   unsigned int attr;
   unsigned int option;
   unsigned int waitType;
   unsigned int waitId;
   unsigned int wakeupCount;
} AuroraPceEeThreadStatusT;

extern int CreateSema(void *);
extern int DeleteSema(int);
extern int SignalSema(int);
extern int WaitSema(int);
extern int CreateThread(void *);
extern int DeleteThread(int);
extern int StartThread(int, void *);
extern int ReferThreadStatus(int, void *);
/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901 */
extern int DelayThread(unsigned int);
extern void *_gp;

#define AURORA_PCE_EE_SYNC() __asm__ __volatile__("sync")
#endif

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

/* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830 */
#define AURORA_PCE_ASYNC_BYTES (64 * 1024)
#define AURORA_PCE_ASYNC_CHUNK (8 * 1024)
#define AURORA_PCE_ASYNC_PATH 4096

static uint8_t s_AuroraPceAsyncBuffer[AURORA_PCE_ASYNC_BYTES]
   __attribute__((aligned(64)));
static uint8_t s_AuroraPceAsyncStack[16 * 1024]
   __attribute__((aligned(16)));

static int s_AuroraPceAsyncThreadId = -1;
static int s_AuroraPceAsyncSema = -1;
static uint32_t s_AuroraPceAsyncSerialCounter;

static volatile uint32_t s_AuroraPceAsyncReqSeq;
static volatile uint32_t s_AuroraPceAsyncReqSerial;
static volatile int64_t s_AuroraPceAsyncReqStart;
static char s_AuroraPceAsyncReqPath[AURORA_PCE_ASYNC_PATH];

static volatile uint32_t s_AuroraPceAsyncBufSerial;
static volatile int64_t s_AuroraPceAsyncBufStart;
static volatile uint64_t s_AuroraPceAsyncBufReady;

/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901
 * Request != 0 gates new async work; Ack is written by the worker only
 * after an in-flight read has returned and its private RFILE is closed. */
static volatile uint32_t s_AuroraPceAsyncPauseRequest;
static volatile uint32_t s_AuroraPceAsyncPauseAck;
static uint32_t s_AuroraPceAsyncPauseCounter;

static void PCE_AuroraCdAsyncThread(void *arg)
{
   uint32_t handled_seq = 0;
   uint32_t active_seq = 0;
   int64_t active_start = 0;
   int64_t worker_pos = 0;
   char active_path[AURORA_PCE_ASYNC_PATH];
   char open_path[AURORA_PCE_ASYNC_PATH];
   RFILE *worker_file = NULL;

   (void)arg;
   active_path[0] = 0;
   open_path[0] = 0;

   for (;;)
   {
      uint32_t seq;
      uint64_t done;
      size_t want;
      int64_t got;

      WaitSema(s_AuroraPceAsyncSema);

      /* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901 */
      if (s_AuroraPceAsyncPauseRequest != 0)
      {
         uint32_t pause_token = s_AuroraPceAsyncPauseRequest;

         if (worker_file)
            filestream_close(worker_file);
         worker_file = NULL;
         open_path[0] = 0;
         active_path[0] = 0;
         active_seq = 0;
         worker_pos = 0;

         AURORA_PCE_EE_SYNC();
         s_AuroraPceAsyncPauseAck = pause_token;
         AURORA_PCE_EE_SYNC();
         continue;
      }

      seq = s_AuroraPceAsyncReqSeq;

      if (seq != handled_seq)
      {
         handled_seq = seq;
         active_seq = seq;
         active_start = s_AuroraPceAsyncReqStart;
         strncpy(active_path, s_AuroraPceAsyncReqPath,
                 sizeof(active_path) - 1);
         active_path[sizeof(active_path) - 1] = 0;

         if (!active_path[0])
         {
            if (worker_file)
               filestream_close(worker_file);
            worker_file = NULL;
            open_path[0] = 0;
            continue;
         }

         if (!worker_file || strcmp(open_path, active_path))
         {
            if (worker_file)
               filestream_close(worker_file);
            worker_file = filestream_open(
               active_path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE);
            open_path[0] = 0;
            if (worker_file)
            {
               /* Separate RFILE/VFS handle; no stdio buffering state shared. */
               strncpy(open_path, active_path, sizeof(open_path) - 1);
               open_path[sizeof(open_path) - 1] = 0;
            }
         }

         if (!worker_file ||
             filestream_seek(worker_file, active_start,
                            RETRO_VFS_SEEK_POSITION_START) < 0)
         {
            if (worker_file)
            {
               filestream_close(worker_file);
               worker_file = NULL;
               open_path[0] = 0;
            }
            continue;
         }
         worker_pos = active_start;
      }

      if (!worker_file || active_seq != s_AuroraPceAsyncReqSeq)
         continue;

      done = s_AuroraPceAsyncBufReady;
      if (done >= AURORA_PCE_ASYNC_BYTES)
         continue;

      if (worker_pos != active_start + (int64_t)done)
      {
         if (filestream_seek(
                   worker_file, active_start + (int64_t)done,
                   RETRO_VFS_SEEK_POSITION_START) < 0)
            continue;
         worker_pos = active_start + (int64_t)done;
      }

      want = (size_t)(AURORA_PCE_ASYNC_BYTES - done);
      if (want > AURORA_PCE_ASYNC_CHUNK)
         want = AURORA_PCE_ASYNC_CHUNK;

      got = filestream_read(
         worker_file, s_AuroraPceAsyncBuffer + (size_t)done,
         (int64_t)want);

      if (active_seq != s_AuroraPceAsyncReqSeq)
         continue;

      if (got > 0)
      {
         worker_pos += (int64_t)got;
         AURORA_PCE_EE_SYNC();
         s_AuroraPceAsyncBufReady = done + (uint64_t)got;
      }
   }
}

static int PCE_AuroraCdAsyncEnsureThread(void)
{
   AuroraPceEeSemaT sema;
   AuroraPceEeThreadT thread;
   AuroraPceEeThreadStatusT current;
   int priority = 40;

   if (s_AuroraPceAsyncThreadId >= 0 && s_AuroraPceAsyncSema >= 0)
      return 1;

   memset(&sema, 0, sizeof(sema));
   sema.init_count = 0;
   sema.max_count = 1;
   s_AuroraPceAsyncSema = CreateSema(&sema);
   if (s_AuroraPceAsyncSema < 0)
      return 0;

   memset(&current, 0, sizeof(current));
   if (ReferThreadStatus(0, &current) >= 0)
   {
      priority = current.current_priority;
      if (priority > 1)
         --priority;
   }

   memset(&thread, 0, sizeof(thread));
   thread.func = (void *)PCE_AuroraCdAsyncThread;
   thread.stack = s_AuroraPceAsyncStack;
   thread.stack_size = sizeof(s_AuroraPceAsyncStack);
   thread.gp_reg = &_gp;
   thread.initial_priority = priority;

   s_AuroraPceAsyncThreadId = CreateThread(&thread);
   if (s_AuroraPceAsyncThreadId < 0)
   {
      DeleteSema(s_AuroraPceAsyncSema);
      s_AuroraPceAsyncSema = -1;
      return 0;
   }

   if (StartThread(s_AuroraPceAsyncThreadId, NULL) < 0)
   {
      DeleteThread(s_AuroraPceAsyncThreadId);
      DeleteSema(s_AuroraPceAsyncSema);
      s_AuroraPceAsyncThreadId = -1;
      s_AuroraPceAsyncSema = -1;
      return 0;
   }

   return 1;
}

static void PCE_AuroraCdAsyncSignal(void)
{
   if (s_AuroraPceAsyncSema >= 0)
      (void)SignalSema(s_AuroraPceAsyncSema);
}

static void PCE_AuroraCdAsyncReset(cdstream *s, int64_t start)
{
   if (s_AuroraPceAsyncPauseRequest ||
       !s || !s->ps2_async_serial || !s->ps2_async_path ||
       !*s->ps2_async_path || !PCE_AuroraCdAsyncEnsureThread())
      return;

   s_AuroraPceAsyncReqSerial = s->ps2_async_serial;
   s_AuroraPceAsyncReqStart = start;
   strncpy(s_AuroraPceAsyncReqPath, s->ps2_async_path,
           sizeof(s_AuroraPceAsyncReqPath) - 1);
   s_AuroraPceAsyncReqPath[sizeof(s_AuroraPceAsyncReqPath) - 1] = 0;

   s_AuroraPceAsyncBufSerial = s->ps2_async_serial;
   s_AuroraPceAsyncBufStart = start;
   s_AuroraPceAsyncBufReady = 0;
   AURORA_PCE_EE_SYNC();
   ++s_AuroraPceAsyncReqSeq;
   PCE_AuroraCdAsyncSignal();
}

static void PCE_AuroraCdAsyncKick(cdstream *s, int64_t pos)
{
   if (s_AuroraPceAsyncPauseRequest ||
       !s || !s->ps2_async_serial || !s->ps2_async_path)
      return;

   if (s_AuroraPceAsyncBufSerial != s->ps2_async_serial ||
       pos < s_AuroraPceAsyncBufStart ||
       pos >= s_AuroraPceAsyncBufStart + AURORA_PCE_ASYNC_BYTES)
   {
      PCE_AuroraCdAsyncReset(s, pos);
      return;
   }

   if (s_AuroraPceAsyncBufReady < AURORA_PCE_ASYNC_BYTES)
      PCE_AuroraCdAsyncSignal();
}

void PCE_AuroraCdAsyncForget(uint32_t serial)
{
   if (!serial)
      return;

   if (s_AuroraPceAsyncBufSerial == serial ||
       s_AuroraPceAsyncReqSerial == serial)
   {
      s_AuroraPceAsyncBufSerial = 0;
      s_AuroraPceAsyncBufReady = 0;
      s_AuroraPceAsyncReqSerial = 0;
      s_AuroraPceAsyncReqPath[0] = 0;
      AURORA_PCE_EE_SYNC();
      ++s_AuroraPceAsyncReqSeq;
      PCE_AuroraCdAsyncSignal();
   }
}

static void PCE_AuroraCdAsyncCancelAll(void)
{
   s_AuroraPceAsyncBufSerial = 0;
   s_AuroraPceAsyncBufReady = 0;
   s_AuroraPceAsyncReqSerial = 0;
   s_AuroraPceAsyncReqPath[0] = 0;
   AURORA_PCE_EE_SYNC();
   ++s_AuroraPceAsyncReqSeq;
   PCE_AuroraCdAsyncSignal();
}

/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901
 * Stop async CDDA filesystem traffic and wait for worker acknowledgement.
 * Logical cdstream positions are not rewound. */
int PCE_AuroraCdAsyncQuiesce(unsigned int timeout_ms)
{
   uint32_t token;
   unsigned int waited = 0;

   if (s_AuroraPceAsyncThreadId < 0 || s_AuroraPceAsyncSema < 0)
      return 1;

   token = ++s_AuroraPceAsyncPauseCounter;
   if (token == 0)
      token = ++s_AuroraPceAsyncPauseCounter;

   s_AuroraPceAsyncBufSerial = 0;
   s_AuroraPceAsyncBufReady = 0;
   s_AuroraPceAsyncReqSerial = 0;
   s_AuroraPceAsyncReqPath[0] = 0;
   s_AuroraPceAsyncPauseAck = 0;
   s_AuroraPceAsyncPauseRequest = token;
   AURORA_PCE_EE_SYNC();

   ++s_AuroraPceAsyncReqSeq;
   PCE_AuroraCdAsyncSignal();

   for (;;)
   {
      AURORA_PCE_EE_SYNC();
      if (s_AuroraPceAsyncPauseAck == token)
         return 1;
      if (waited >= timeout_ms)
         break;
      DelayThread(1000);
      ++waited;
   }

   AURORA_PCE_EE_SYNC();
   return s_AuroraPceAsyncPauseAck == token ? 1 : 0;
}

void PCE_AuroraCdAsyncResume(void)
{
   s_AuroraPceAsyncPauseRequest = 0;
   s_AuroraPceAsyncPauseAck = 0;
   AURORA_PCE_EE_SYNC();
}

/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
static int s_AuroraPceCdAudioSafeWindow;
static int s_AuroraPceCdAudioRefillRequested;
/* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830 */
static cdstream *s_AuroraPceCdAudioPendingStream;
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
static int s_AuroraPceCdMusicEnabled = 1;

void PCE_AuroraSetCdAudioSafeWindow(int allowed)
{
   s_AuroraPceCdAudioSafeWindow = allowed ? 1 : 0;
}

int PCE_AuroraConsumeCdAudioRefillRequest(void)
{
   int requested = s_AuroraPceCdAudioRefillRequested;
   s_AuroraPceCdAudioRefillRequested = 0;
   return requested;
}

int PCE_AuroraCdAudioSafeWindow(void)
{
   return s_AuroraPceCdAudioSafeWindow;
}

void PCE_AuroraRequestCdAudioRefill(cdstream *s)
{
   s_AuroraPceCdAudioRefillRequested = 1;
   /* NULL is intentional for CHD: clear any stale raw-stream prefetch. */
   s_AuroraPceCdAudioPendingStream = s;
}

/* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830
 * Prefill raw CDDA cache without advancing logical s->pos. */
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
void PCE_AuroraSetCdMusicEnabled(int enabled)
{
   s_AuroraPceCdMusicEnabled = enabled ? 1 : 0;
   if (!s_AuroraPceCdMusicEnabled)
   {
      s_AuroraPceCdAudioSafeWindow = 0;
      s_AuroraPceCdAudioRefillRequested = 0;
      s_AuroraPceCdAudioPendingStream = NULL;
      /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830 */
      PCE_AuroraCdAsyncCancelAll();
   }
}

int PCE_AuroraCdMusicEnabled(void)
{
   return s_AuroraPceCdMusicEnabled;
}

int PCE_AuroraPrefetchCdAudio(void)
{
   /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830 */
   return 0;
}

void cdstream_ps2_cache_forget(cdstream *s)
{
   /* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830 */
   if (s_AuroraPceCdAudioPendingStream == s)
      s_AuroraPceCdAudioPendingStream = NULL;

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

/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
uint64_t cdstream_ps2_read_audio_failsoft(cdstream *s, void *data,
                                          uint64_t count)
{
   uint8_t *out = (uint8_t *)data;
   uint64_t total = 0;

   if (!s || !data || count == 0)
      return 0;

   if (!s_AuroraPceCdMusicEnabled)
   {
      uint64_t drop = count;
      if (s->size > 0 && s->pos >= 0 &&
          s->size - (uint64_t)s->pos < drop)
         drop = s->size - (uint64_t)s->pos;
      memset(data, 0, (size_t)drop);
      if (s->pos >= 0)
         s->pos += (int64_t)drop;
      return drop;
   }

   if (s->buf)
      return cdstream_read(s, data, count);

   while (count > 0)
   {
      uint64_t available = 0;
      uint64_t take = 0;

      if (s->pos < 0)
         break;
      if (s->size > 0 && (uint64_t)s->pos >= s->size)
         break;

      if (s_AuroraPceAsyncBufSerial == s->ps2_async_serial &&
          s->pos >= s_AuroraPceAsyncBufStart &&
          s->pos < s_AuroraPceAsyncBufStart +
                   (int64_t)s_AuroraPceAsyncBufReady)
      {
         available = s_AuroraPceAsyncBufReady -
            (uint64_t)(s->pos - s_AuroraPceAsyncBufStart);
         take = available < count ? available : count;
      }

      if (take > 0)
      {
         memcpy(out,
                s_AuroraPceAsyncBuffer +
                   (size_t)(s->pos - s_AuroraPceAsyncBufStart),
                (size_t)take);
         out += take;
         count -= take;
         total += take;
         s->pos += (int64_t)take;
         PCE_AuroraCdAsyncKick(s, s->pos);
         continue;
      }

      {
         uint64_t drop = count;
         if (s->size > 0 && s->size - (uint64_t)s->pos < drop)
            drop = s->size - (uint64_t)s->pos;
         if (drop == 0)
            break;

         PCE_AuroraCdAsyncKick(s, s->pos);
         memset(out, 0, (size_t)drop);
         out += drop;
         count -= drop;
         total += drop;
         s->pos += (int64_t)drop;
      }
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
      /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830 */
      out->ps2_async_serial = ++s_AuroraPceAsyncSerialCounter;
      if (out->ps2_async_serial == 0)
         out->ps2_async_serial = ++s_AuroraPceAsyncSerialCounter;
      if (path)
      {
         size_t plen = strlen(path);
         out->ps2_async_path = (char *)malloc(plen + 1);
         if (out->ps2_async_path)
            memcpy(out->ps2_async_path, path, plen + 1);
      }
      /* Worker starts lazily only if Red Book audio actually misses RAM. */
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

/* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830 */
