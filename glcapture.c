/* gcc -std=c99 -fPIC -shared -Wl,-soname,glcapture.so glcapture.c -lasound -o glcapture.so
 * gcc -m32 -std=c99 -fPIC -shared -Wl,-soname,glcapture.so glcapture.c -lasound -o glcapture.so (for 32bit)
 *
 * Capture OpenGL framebuffer, ALSA audio and push them through named pipe
 * Usage: LD_PRELOAD="/path/to/glcapture.so" ./program
 *
 * https://github.com/Cloudef/FFmpeg/tree/rawmux
 * ^ Compile this branch of ffmpeg to get rawmux decoder
 * You can test that it works by doing ./ffplay /tmp/glcapture.fifo
 *
 * Make sure you increase your maximum pipe size /prox/sys/fs/pipe-max-size to minimum of
 * (FPS / 4) * ((width * height * components) + 13) where components is 3 on OpenGL and 4 on OpenGL ES.
 * Also set /proc/sys/fs/pipe-user-pages-soft to 0.
 *
 * If you get xruns from alsa, consider increasing your audio buffer size.
 */

/**
 * TODO:
 * - Consider alternative such as using DRM/VAAPI to encode directly to pipe
 * - NVENC also exists for nv blob, however seems to not have public GL interop
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <err.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <GL/glx.h>
#include <EGL/egl.h>
#include <alsa/asoundlib.h>

// Some tunables
// XXX: Make these configurable

// Use any amount you want as long as you have the vram for it
// If you get warning of map_buffer taking time, try increasing this
#define NUM_PBOS 4

// Target framerate for the video stream
static uint32_t TARGET_FPS = 60;

// Drop frames if going over target framerate
// Set this to false if you want frame perfect capture
// If your target framerate is lower than game framerate set this to true (i.e. you want to record at lower fps)
static bool DROP_FRAMES = true;

// Multiplier for system clock. Can be used to make recordings of replays smoother (or speed hack)
static double SPEED_HACK = 1.0;

// If your video is upside down set this to false
static bool FLIP_VIDEO = true;

// Path for the fifo where glcapture will output the rawmux data
static const char *FIFO_PATH = "/tmp/glcapture.fifo";

enum stream {
   STREAM_VIDEO,
   STREAM_AUDIO,
   STREAM_LAST,
};

// Set to false to disable stream
static const bool ENABLED_STREAMS[STREAM_LAST] = {
   true, // STREAM_VIDEO
   true, // STREAM_AUDIO
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define WARN(x, ...) do { warn("glcapture: "x, ##__VA_ARGS__); } while (0)
#define WARNX(x, ...) do { warnx("glcapture: "x, ##__VA_ARGS__); } while (0)
#define ERRX(x, y, ...) do { errx(x, "glcapture: "y, ##__VA_ARGS__); } while (0)
#define ERR(x, y, ...) do { err(x, "glcapture: "y, ##__VA_ARGS__); } while (0)
#define WARN_ONCE(x, ...) do { static bool o = false; if (!o) { WARNX(x, ##__VA_ARGS__); o = true; } } while (0)

// "entrypoints" exposed to hooks.h
static void swap_buffers(void);
static void alsa_writei(snd_pcm_t *pcm, const void *buffer, const snd_pcm_uframes_t size, const char *caller);
static uint64_t get_fake_time_ns(clockid_t clk_id);
static __thread GLint LAST_FRAMEBUFFER_BLIT[8];

#include "hooks.h"
#include "glwrangle.h"

struct pbo {
   uint64_t ts;
   uint32_t width, height;
   GLuint obj;
   bool written;
};

struct gl {
   struct pbo pbo[NUM_PBOS];
   uint8_t active; // pbo
};

struct frame_info {
   union {
      struct {
         uint32_t width, height, fps;
      } video;
      struct {
         uint32_t rate;
         uint8_t channels;
      } audio;
   };

   const char *format;
   uint64_t ts;
   enum stream stream;
};

struct fifo {
   struct {
      struct frame_info info;
   } stream[STREAM_LAST];

   FILE *file;
   uint64_t base;
   size_t size;
   int fd;
   bool created;
};

struct buffer {
   void *data;
   size_t size, allocated;
};

#define PROFILE(x, warn_ms, name) do { \
   const uint64_t start = get_time_ns_clock(CLOCK_PROCESS_CPUTIME_ID); \
   x; \
   const double ms = (get_time_ns_clock(CLOCK_PROCESS_CPUTIME_ID) - start) / 1e6; \
   if (ms >= warn_ms) WARNX("WARNING: %s took %.2f ms (>=%.0fms)", name, ms, warn_ms); \
} while (0)

static void
buffer_resize(struct buffer *buffer, const size_t size)
{
   if (buffer->allocated < size) {
      if (!(buffer->data = realloc(buffer->data, size)))
         ERR(EXIT_FAILURE, "realloc(%p, %zu)", buffer->data, size);

      buffer->allocated = size;
   }

   buffer->size = size;
}

static uint64_t
get_time_ns_clock(clockid_t clk_id)
{
   struct timespec ts;
   HOOK(clock_gettime);
   _clock_gettime(clk_id, &ts);
   return (uint64_t)ts.tv_sec * (uint64_t)1e9 + (uint64_t)ts.tv_nsec;
}

static uint64_t
get_time_ns(void)
{
   return get_time_ns_clock(CLOCK_MONOTONIC_COARSE);
}

static void
reset_fifo(struct fifo *fifo)
{
   close(fifo->fd);
   memset(fifo, 0, sizeof(*fifo));
   fifo->fd = -1;
   WARNX("reseting fifo");
}

static bool
write_rawmux_header(struct fifo *fifo)
{
   uint8_t header[255] = { 'r', 'a', 'w', 'm', 'u', 'x' };

   size_t variable_sz = 0;
   for (enum stream i = 0; i < STREAM_LAST; ++i)
      variable_sz += (fifo->stream[i].info.format ? strlen(fifo->stream[i].info.format) : 0);

   if (variable_sz + 33 > sizeof(header)) {
      warnx("something went wrong");
      reset_fifo(fifo);
      return false;
   }

   uint8_t *p = header + 6;
   memcpy(p, (uint8_t[]){1}, sizeof(uint8_t)); p += 1;

   if (fifo->stream[STREAM_VIDEO].info.format) {
      const struct frame_info *info = &fifo->stream[STREAM_VIDEO].info;
      memcpy(p, (uint8_t[]){1}, sizeof(uint8_t)); p += 1;
      memcpy(p, info->format, strlen(info->format)); p += strlen(info->format) + 1;
      memcpy(p, (uint32_t[]){1}, sizeof(uint32_t)); p += 4;
      memcpy(p, (uint32_t[]){info->video.fps * 1000}, sizeof(uint32_t)); p += 4;
      memcpy(p, &info->video.width, sizeof(uint32_t)); p += 4;
      memcpy(p, &info->video.height, sizeof(uint32_t)); p += 4;
   }

   if (fifo->stream[STREAM_AUDIO].info.format) {
      const struct frame_info *info = &fifo->stream[STREAM_AUDIO].info;
      memcpy(p, (uint8_t[]){2}, sizeof(uint8_t)); p += 1;
      memcpy(p, info->format, strlen(info->format)); p += strlen(info->format) + 1;
      memcpy(p, &info->audio.rate, sizeof(info->audio.rate)); p += 4;
      memcpy(p, &info->audio.channels, sizeof(info->audio.channels)); p += 1;
   }

   return (fwrite(header, 1, (p + 1) - header, fifo->file) == (size_t)((p + 1) - header));
}

static bool
stream_info_changed(const struct frame_info *current, const struct frame_info *last)
{
   assert(current->stream == last->stream);

   if (current->stream == STREAM_VIDEO) {
      return (current->format != last->format ||
              current->video.width != last->video.width ||
              current->video.height != last->video.height);
   }

   return (current->format != last->format ||
           current->audio.rate != last->audio.rate ||
           current->audio.channels != last->audio.channels);
}

static bool
check_and_prepare_stream(struct fifo *fifo, const struct frame_info *info)
{
   if (!ENABLED_STREAMS[info->stream])
      return false;

   if (fifo->stream[info->stream].info.format && stream_info_changed(info, &fifo->stream[info->stream].info)) {
      WARNX("stream information has changed");
      reset_fifo(fifo);
   }

   fifo->stream[info->stream].info = *info;

   if (!fifo->created) {
      remove(FIFO_PATH);

      if (!(fifo->created = !mkfifo(FIFO_PATH, 0666)))
         return false;

      fifo->created = true;
   }

   if (fifo->fd < 0) {
      signal(SIGPIPE, SIG_IGN);

      if ((fifo->fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
         return false;

      // We will use fwrite instead of write for buffered writes.
      // Which will be more stable, since audio/video data isn't actually that large per frame.
      // We also avoid calling to kernel each call.
      fifo->file = fdopen(fifo->fd, "wb");
      assert(fifo->file);

      const int flags = fcntl(fifo->fd, F_GETFL);
      fcntl(fifo->fd, F_SETFL, flags & ~O_NONBLOCK);
      WARNX("stream ready, writing headers");

      if (!write_rawmux_header(fifo))
         return false;

      fifo->base = info->ts;
   }

   return true;
}

static void
write_data_unsafe(struct fifo *fifo, const struct frame_info *info, const void *buffer, const size_t size)
{
   if (!check_and_prepare_stream(fifo, info) || info->ts < fifo->base)
      return;

   const uint64_t den[STREAM_LAST] = { 1e6, 1e9 };
   const uint64_t rate = (info->stream == STREAM_VIDEO ? info->video.fps : info->audio.rate);
   const uint64_t pts = (info->ts - fifo->base) / (den[info->stream] / rate);

#if 0
   WARNX("PTS: (%u) %llu", info->stream, pts);
#endif

   uint8_t frame[] = {
      info->stream,
      0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0
   };

   memcpy(frame + 1, (uint32_t[]){size}, sizeof(uint32_t));
   memcpy(frame + 1 + 4, (uint64_t[]){pts}, sizeof(uint64_t));

   {
      const size_t pipe_sz = (TARGET_FPS / 4) * (size + sizeof(frame));

      if (fifo->size < pipe_sz) {
         if (fcntl(fifo->fd, F_SETPIPE_SZ, pipe_sz) == -1) {
            WARN("fcntl(F_SETPIPE_SZ, %zu) (%u)", pipe_sz, info->stream);
            reset_fifo(fifo);
            return;
         }

         fifo->size = pipe_sz;

         // Set some reasonable buffer size for fwrites
         // This is still experimental and if you get smoother output by setting this to
         // _IONBF let me know.
         setvbuf(fifo->file, NULL, _IOFBF, fifo->size / 8);
      }
   }

   errno = 0;
   size_t ret;
   if ((ret = fwrite(frame, 1, sizeof(frame), fifo->file) != sizeof(frame)) ||
      ((ret = fwrite(buffer, 1, size, fifo->file)) != size)) {
      WARN("write(%zu) (%u)", ret, info->stream);
      reset_fifo(fifo);
   }
}

static void
write_data(const struct frame_info *info, const void *buffer, const size_t size)
{
   // we need to protect our fifo structure, since games usually output audio on another thread and so
   static struct fifo fifo = { .fd = -1 };
   static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
   pthread_mutex_lock(&mutex);
   write_data_unsafe(&fifo, info, buffer, size);
   pthread_mutex_unlock(&mutex);
}

void
flip_pixels_if_needed(const GLint view[8], uint8_t *pixels, const uint32_t width, const uint32_t height, const uint8_t components)
{
   // Will detect at least wine which blits viewport sized framebuffer at the end already flipped
   if (!FLIP_VIDEO || (view[5] == view[3] && view[6] == view[2]))
      return;

   // Sadly I can't come up with any reliable way to do this on GPU on all possible OpenGL versions and variants.
   // FIXME: This function however is quite expensive and causes capture to take more than 1ms easily.
   //        Should try dig deeper and see how I could make GPU do the flip without having to read twice.

   const uint32_t stride = width * components;
   static __thread struct buffer row;
   buffer_resize(&row, stride);

   for (uint8_t *lo = pixels, *hi = pixels + (height - 1) * stride; lo < hi; lo += stride, hi -= stride) {
      memcpy(row.data, lo, stride);
      memcpy(lo, hi, stride);
      memcpy(hi, row.data, stride);
   }
}

static bool
is_buffer(GLuint obj)
{
   return (obj > 0 && glIsBuffer(obj));
}

static void
capture_frame_pbo(struct gl *gl, const GLint view[8], const uint64_t ts)
{
   const struct {
      const char *video;
      GLenum format;
      uint8_t components;
   } frame = {
      // XXX: Maybe on ES we should instead modify the data and remove A component?
      //      Would save some transmission bandwidth at least (from GPU and to PIPE)
      //      RGB also is unaligned, but seem just as fast as RGBA on Nvidia.
      .video = (OPENGL_VARIANT == OPENGL_ES ? "rgb0" : "rgb"),
      .format = (OPENGL_VARIANT == OPENGL_ES ? GL_RGBA : GL_RGB),
      .components = (OPENGL_VARIANT == OPENGL_ES ? 4 : 3),
   };

   if (!is_buffer(gl->pbo[gl->active].obj)) {
      WARNX("create pbo %u", gl->active);
      glGenBuffers(1, &gl->pbo[gl->active].obj);
   }

   struct { GLenum t; GLint o; GLint v; } map[] = {
      { .t = GL_PACK_ALIGNMENT, .v = 1 },
      { .t = GL_PACK_ROW_LENGTH },
      { .t = GL_PACK_IMAGE_HEIGHT },
      { .t = GL_PACK_SKIP_PIXELS },
   };

   PROFILE(
   glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[gl->active].obj);
   glBufferData(GL_PIXEL_PACK_BUFFER, view[2] * view[3] * frame.components, NULL, GL_STREAM_READ);

   for (size_t i = 0; i < ARRAY_SIZE(map); ++i) {
      glGetIntegerv(map[i].t, &map[i].o);
      glPixelStorei(map[i].t, map[i].v);
   }

   glReadPixels(view[0], view[1], view[2], view[3], frame.format, GL_UNSIGNED_BYTE, NULL);
   glFlush();

   for (size_t i = 0; i < ARRAY_SIZE(map); ++i)
      glPixelStorei(map[i].t, map[i].o);

   gl->pbo[gl->active].ts = ts;
   gl->pbo[gl->active].width = view[2];
   gl->pbo[gl->active].height = view[3];
   gl->pbo[gl->active].written = (glGetError() == GL_NO_ERROR);
   , 1.0, "read_frame");

   gl->active = (gl->active + 1) % NUM_PBOS;

   if (is_buffer(gl->pbo[gl->active].obj) && gl->pbo[gl->active].written) {
      const struct frame_info info = {
         .ts = gl->pbo[gl->active].ts,
         .stream = STREAM_VIDEO,
         .format = frame.video,
         .video.width = gl->pbo[gl->active].width,
         .video.height = gl->pbo[gl->active].height,
         .video.fps = TARGET_FPS,
      };

      void *buf;
      const size_t size = info.video.width * info.video.height * frame.components;

      PROFILE(
      glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[gl->active].obj);
      buf = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT);
      , 2.0, "map_buffer");

      if (buf) {
         PROFILE(
         flip_pixels_if_needed(view, buf, info.video.width, info.video.height, frame.components);
         write_data(&info, buf, size);
         glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
         gl->pbo[gl->active].written = false;
         , 2.0, "write_frame");
      }
   }
}

static void
reset_capture(struct gl *gl)
{
   for (size_t i = 0; i < NUM_PBOS; ++i) {
      if (is_buffer(gl->pbo[i].obj))
         glDeleteBuffers(1, &gl->pbo[i].obj);
   }

   WARNX("capture reset");
   *gl = (struct gl){0};
}

static void
capture_frame(struct gl *gl, const uint64_t ts, const uint32_t fps, const GLint view[8])
{
   static __thread uint64_t last_time;
   const uint64_t target_rate = (1e9 / (TARGET_FPS * 2));
   const uint64_t current_rate = (1e9 / fps);
   const uint64_t rate = target_rate - current_rate;

   if (DROP_FRAMES && last_time > 0 && target_rate > current_rate && ts - last_time <= rate) {
      WARNX("WARNING: dropping frame (%.2f <= %.2f)", (ts - last_time) / 1e6, rate / 1e6);
      return;
   }

   last_time = ts;

   GLint pbo;
   glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
   capture_frame_pbo(gl, view, ts);
   glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
}

static void
draw_indicator(const GLint view[8])
{
   GLfloat clear[4];
   GLboolean scissor;
   glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
   glGetBooleanv(GL_SCISSOR_TEST, &scissor);

   if (!scissor)
      glEnable(GL_SCISSOR_TEST);

   const uint32_t size = (view[3] / 75 > 10 ? view[3] / 75 : 10);
   glScissor(size / 2 - 1, view[3] - size - size / 2 - 1, size + 2, size + 2);
   glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glScissor(size / 2, view[3] - size - size / 2, size, size);
   glClearColor(1.0f, 0.0f, 0.0f, 0.0f);
   glClear(GL_COLOR_BUFFER_BIT);

   if (!scissor)
      glDisable(GL_SCISSOR_TEST);

   glClearColor(clear[0], clear[1], clear[2], clear[3]);
}

static void
swap_buffers(void)
{
   static __thread uint64_t last_time, fps_time;
   const uint64_t ts = get_time_ns();
   const uint32_t fps = (last_time > 0 ? 1.0 / ((ts - last_time) / 1e9) : TARGET_FPS);
   last_time = ts;

   if ((ts - fps_time) / 1e9 > 5.0) {
      WARNX("FPS: %u", fps);
      fps_time = ts;
   }

   void* (*procs[])(const char*) = {
      (void*)_eglGetProcAddress,
      (void*)_glXGetProcAddressARB,
      (void*)_glXGetProcAddress
   };

   load_gl_function_pointers(procs, ARRAY_SIZE(procs));
   while (glGetError() != GL_NO_ERROR);

   PROFILE(
   static __thread struct gl gl;
   GLint view[ARRAY_SIZE(LAST_FRAMEBUFFER_BLIT)];

   if (LAST_FRAMEBUFFER_BLIT[2] == 0 || LAST_FRAMEBUFFER_BLIT[3] == 0) {
      glGetIntegerv(GL_VIEWPORT, view);
   } else {
      memcpy(view, LAST_FRAMEBUFFER_BLIT, sizeof(view));
   }

   PROFILE(capture_frame(&gl, ts, fps, view), 2.0, "capture_frame");
   PROFILE(draw_indicator(view), 1.0, "draw_indicator");

   if (glGetError() != GL_NO_ERROR) {
      WARNX("glError occured");
      reset_capture(&gl);
   }
   , 2.0, "swap_buffers");
}

static const char*
alsa_get_format(const snd_pcm_format_t format)
{
   switch (format) {
      case SND_PCM_FORMAT_FLOAT64_LE: return "f64le";
      case SND_PCM_FORMAT_FLOAT64_BE: return "f64be";
      case SND_PCM_FORMAT_FLOAT_LE: return "f32le";
      case SND_PCM_FORMAT_FLOAT_BE: return "f32be";
      case SND_PCM_FORMAT_S32_LE: return "s32le";
      case SND_PCM_FORMAT_S32_BE: return "s32be";
      case SND_PCM_FORMAT_U32_LE: return "u32le";
      case SND_PCM_FORMAT_U32_BE: return "u32be";
      case SND_PCM_FORMAT_S24_LE: return "s24le";
      case SND_PCM_FORMAT_S24_BE: return "s24be";
      case SND_PCM_FORMAT_U24_LE: return "u24le";
      case SND_PCM_FORMAT_U24_BE: return "u24be";
      case SND_PCM_FORMAT_S16_LE: return "s16le";
      case SND_PCM_FORMAT_S16_BE: return "s16be";
      case SND_PCM_FORMAT_U16_LE: return "u16le";
      case SND_PCM_FORMAT_U16_BE: return "u16be";
      case SND_PCM_FORMAT_S8: return "s8";
      case SND_PCM_FORMAT_U8: return "u8";
      case SND_PCM_FORMAT_MU_LAW: return "mulaw";
      case SND_PCM_FORMAT_A_LAW: return "alaw";
      default: break;
   }

   WARN_ONCE("can't convert alsa format: %u", format);
   return NULL;
}

static bool
alsa_get_frame_info(snd_pcm_t *pcm, struct frame_info *out_info, const char *caller)
{
   snd_pcm_format_t format;
   unsigned int channels, rate;
   snd_pcm_hw_params_t *params = alloca(snd_pcm_hw_params_sizeof());
   snd_pcm_hw_params_current(pcm, params);
   snd_pcm_hw_params_get_format(params, &format);
   snd_pcm_hw_params_get_channels(params, &channels);
   snd_pcm_hw_params_get_rate(params, &rate, NULL);
   WARN_ONCE("%s (%s:%u:%u)", caller, snd_pcm_format_name(format), rate, channels);
   out_info->ts = get_time_ns();
   out_info->stream = STREAM_AUDIO;
   out_info->format = alsa_get_format(format);
   out_info->audio.rate = rate;
   out_info->audio.channels = channels;
   return (out_info->format != NULL);
}

static void
alsa_writei(snd_pcm_t *pcm, const void *buffer, const snd_pcm_uframes_t size, const char *caller)
{
   struct frame_info info;
   if (alsa_get_frame_info(pcm, &info, caller))
      PROFILE(write_data(&info, buffer, snd_pcm_frames_to_bytes(pcm, size)), 2.0, "alsa_write");
}

static uint64_t
get_fake_time_ns(clockid_t clk_id)
{
   static __thread uint64_t base[16];
   assert((size_t)clk_id < ARRAY_SIZE(base));
   const uint64_t current = get_time_ns_clock(clk_id);
   base[clk_id] = (base[clk_id] ? base[clk_id] : current);
   return base[clk_id] + (current - base[clk_id]) * SPEED_HACK;
}
