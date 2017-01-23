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
 *
 * If you get xruns from alsa, consider increasing your audio buffer size.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
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
#define NUM_PBOS 2

// Target framerate for the video stream
static uint32_t FPS = 60;

// Drop frames if going over target framerate
// Set this to false if you want frame perfect capture
// If your target framerate is lower than game framerate set this to true (i.e. you want to record at lower fps)
static bool DROP_FRAMES = true;

// Multiplier for system clock (MONOTONIC, RAW) can be used to make recordings of replays smoother (or speed hack)
static double SPEED_HACK = 1.0;

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
#define WARN_ONCE(x, ...) do { static bool o = false; if (!o) { WARNX(x, ##__VA_ARGS__); o = true; } } while (0)

// "entrypoints" exposed to hooks.h
static void swap_buffers(void);
static void alsa_writei(snd_pcm_t *pcm, const void *buffer, const snd_pcm_uframes_t size, const char *caller);
static uint64_t get_fake_time_ns(void);

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

   uint64_t base;
   size_t size;
   int fd;
   bool created;
};

static uint64_t
get_time_ns(void)
{
   struct timespec ts;
   HOOK(clock_gettime);
   _clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * (uint64_t)1e9 + (uint64_t)ts.tv_nsec;
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

   return (write(fifo->fd, header, (p + 1) - header) == (p + 1) - header);
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
      if ((fifo->fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
         return false;

      const int flags = fcntl(fifo->fd, F_GETFL);
      fcntl(fifo->fd, F_SETFL, flags & ~O_NONBLOCK);
      WARNX("stream ready, writing headers");

      if (!write_rawmux_header(fifo))
         return false;

      fifo->base = get_time_ns();
   }

   return true;
}

static void
write_data_unsafe(struct fifo *fifo, const struct frame_info *info, const void *buffer, const size_t size)
{
   if (!check_and_prepare_stream(fifo, info))
      return;

   const uint64_t ts = (fifo->base > info->ts ? fifo->base : info->ts);
   const uint64_t den[STREAM_LAST] = { 1e6, 1e9 };
   const uint64_t rate = (info->stream == STREAM_VIDEO ? info->video.fps : info->audio.rate);
   const uint64_t pts = (ts - fifo->base) / (den[info->stream] / rate);

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
      const size_t pipe_sz = (FPS / 4) * (size + sizeof(frame));

      if (fifo->size < pipe_sz) {
         if (fcntl(fifo->fd, F_SETPIPE_SZ, pipe_sz) == -1) {
            WARN("fcntl(F_SETPIPE_SZ, %zu) (%u)", pipe_sz, info->stream);
            reset_fifo(fifo);
            return;
         }

         fifo->size = pipe_sz;
      }
   }

   errno = 0;
   ssize_t ret;
   if ((ret = write(fifo->fd, frame, sizeof(frame)) != (ssize_t)sizeof(frame)) ||
      ((ret = write(fifo->fd, buffer, size)) != (ssize_t)size)) {
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

static void
capture_frame_pbo(struct gl *gl, const GLint view[4], const uint64_t ts)
{
   const struct {
      const char *video;
      GLenum format;
      uint8_t components;
   } frame = {
      // XXX: Maybe on ES we should instead modify the data and remove A component?
      //      Would save some transmission bandwidth at least
      .video = (OPENGL_VARIANT == OPENGL_ES ? "rgba" : "rgb"),
      .format = (OPENGL_VARIANT == OPENGL_ES ? GL_RGBA : GL_RGB),
      .components = (OPENGL_VARIANT == OPENGL_ES ? 4 : 3),
   };

   if (!glIsBuffer(gl->pbo[gl->active].obj)) {
      WARNX("create pbo %u", gl->active);
      glGenBuffers(1, &gl->pbo[gl->active].obj);
   }

   glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[gl->active].obj);
   glBufferData(GL_PIXEL_PACK_BUFFER, view[2] * view[3] * frame.components, NULL, GL_STREAM_READ);

   struct { GLenum t; GLint o; GLint v; } map[] = {
      { .t = GL_PACK_ALIGNMENT, .v = 1 },
      { .t = GL_PACK_ROW_LENGTH },
      { .t = GL_PACK_IMAGE_HEIGHT },
      { .t = GL_PACK_SKIP_PIXELS },
   };

   for (size_t i = 0; i < ARRAY_SIZE(map); ++i) {
      glGetIntegerv(map[i].t, &map[i].o);
      glPixelStorei(map[i].t, map[i].v);
   }

   glReadPixels(view[0], view[1], view[2], view[3], frame.format, GL_UNSIGNED_BYTE, NULL);

   for (size_t i = 0; i < ARRAY_SIZE(map); ++i)
      glPixelStorei(map[i].t, map[i].o);

   gl->pbo[gl->active].ts = ts;
   gl->pbo[gl->active].width = view[2];
   gl->pbo[gl->active].height = view[3];
   gl->pbo[gl->active].written = true;

   gl->active = (gl->active + 1) % NUM_PBOS;

   if (glIsBuffer(gl->pbo[gl->active].obj) && gl->pbo[gl->active].written) {
      const struct frame_info info = {
         .ts = gl->pbo[gl->active].ts,
         .stream = STREAM_VIDEO,
         .format = frame.video,
         .video.width = gl->pbo[gl->active].width,
         .video.height = gl->pbo[gl->active].height,
         .video.fps = FPS,
      };

      const void *buf;
      const size_t size = info.video.width * info.video.height * frame.components;
      glBindBuffer(GL_PIXEL_PACK_BUFFER, gl->pbo[gl->active].obj);
      if ((buf = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT))) {
         write_data(&info, buf, size);
         glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
         gl->pbo[gl->active].written = false;
      }
   }
}

static void
reset_capture(struct gl *gl)
{
   for (size_t i = 0; i < NUM_PBOS; ++i) {
      if (glIsBuffer(gl->pbo[i].obj))
         glDeleteBuffers(1, &gl->pbo[i].obj);
   }

   WARNX("capture reset");
   memset(gl->pbo, 0, sizeof(gl->pbo));
   gl->active = 0;
}

static void
capture_frame(struct gl *gl, const GLint view[4])
{
   static __thread uint64_t last_time;
   const uint64_t ts = get_time_ns();
   const uint64_t rate = 1e9 / (FPS * 1.5);

   if (DROP_FRAMES && last_time > 0 && ts - last_time < rate)
      return;

   last_time = ts;

   GLint pbo;
   glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
   capture_frame_pbo(gl, view, ts);
   glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
}

static void
draw_indicator(const GLint view[4])
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
   void* (*procs[])(const char*) = {
      (void*)_eglGetProcAddress,
      (void*)_glXGetProcAddressARB,
      (void*)_glXGetProcAddress
   };

   load_gl_function_pointers(procs, ARRAY_SIZE(procs));

   GLint view[4] = {0};
   static __thread struct gl gl;
   const GLenum error0 = glGetError();
   glGetIntegerv(GL_VIEWPORT, view);
   capture_frame(&gl, view);
   draw_indicator(view);

   if (error0 != glGetError()) {
      WARNX("glError occured");
      reset_capture(&gl);
   }
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
      write_data(&info, buffer, snd_pcm_frames_to_bytes(pcm, size));
}

static uint64_t
get_fake_time_ns(void)
{
   static __thread uint64_t base;
   const uint64_t current = get_time_ns();
   base = (base ? base : current);
   return base + (current - base) * SPEED_HACK;
}
