#pragma once

static void (*_glFlush)(void);
static GLenum (*_glGetError)(void);
static void (*_glGetIntegerv)(GLenum, GLint*);
static void (*_glGetFloatv)(GLenum, GLfloat*);
static void (*_glGetBooleanv)(GLenum, GLboolean*);
static const char* (*_glGetString)(GLenum);
static GLboolean (*_glIsBuffer)(GLuint);
static void (*_glGenBuffers)(GLsizei, GLuint*);
static void (*_glDeleteBuffers)(GLsizei, GLuint*);
static void (*_glBindBuffer)(GLenum, GLuint);
static void (*_glBufferData)(GLenum, GLsizeiptr, const GLvoid*, GLenum);
static void* (*_glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void (*_glUnmapBuffer)(GLenum);
static void (*_glPixelStorei)(GLenum, GLint);
static void (*_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
static void (*_glEnable)(GLenum);
static void (*_glDisable)(GLenum);
static void (*_glScissor)(GLint, GLint, GLsizei, GLsizei);
static void (*_glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
static void (*_glClear)(GLbitfield);
static void (*_glDebugMessageCallback)(GLDEBUGPROC, const void*);

enum gl_variant {
   OPENGL_ES,
   OPENGL,
};

struct gl_version {
   uint32_t major, minor;
};

static enum gl_variant OPENGL_VARIANT;
static struct gl_version OPENGL_VERSION;

#define glFlush _glFlush
#define glGetError _glGetError
#define glGetIntegerv _glGetIntegerv
#define glGetFloatv _glGetFloatv
#define glGetBooleanv _glGetBooleanv
#define glGetString _glGetString
#define glIsBuffer _glIsBuffer
#define glGenBuffers _glGenBuffers
#define glDeleteBuffers _glDeleteBuffers
#define glBindBuffer _glBindBuffer
#define glBufferData _glBufferData
#define glMapBufferRange _glMapBufferRange
#define glUnmapBuffer _glUnmapBuffer
#define glPixelStorei _glPixelStorei
#define glReadPixels _glReadPixels
#define glEnable _glEnable
#define glDisable _glDisable
#define glScissor _glScissor
#define glClearColor _glClearColor
#define glClear _glClear
#define glDebugMessageCallback _glDebugMessageCallback

static void
debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *data)
{
   (void)source, (void)type, (void)id, (void)severity, (void)length,  (void)message, (void)data;
   WARNX("%s", message);
}

static void*
dlsym_proc(const char *procname)
{
   void *ptr = NULL;
   hook_function(&ptr, procname, false, (const char*[]){ GL_LIBS, NULL });
   return ptr;
}

static void
load_gl_function_pointers(void* (*procs[])(const char*), const size_t memb)
{
   static bool loaded;

   if (loaded)
      return;

   void* (*proc)(const char*);
   for (size_t i = 0; i < memb; ++i) {
      if ((proc = procs[i]))
         break;
   }

   if (!proc)
      proc = dlsym_proc;

   // We try to support wide range of OpenGL and variants as possible.
   // Thus avoid using functions that only work in certain OpenGL versions.
   // e.g. glPushAttrib, glPushClientAttrib, it's bit of shitty but such is life.
   // Alternatively if code starts getting too much saving/restoring, consider hooking
   // the gl state changes we care about and write our own push/pop around swap_buffer.
   //
   // Version / variant dependant code is still possible through GL_VARIANT and GL_VERSION variables.
   //
   // Note that we also rely on system GL/glx.h for typedefs / constants, which probably is plain wrong on ES
   // for example, but seems to work fine so far. Main interest is to work with mainly GLX / Wine games anyways.

#define GL_REQUIRED(x) do { if (!(_##x = proc(#x))) { ERRX(EXIT_FAILURE, "Failed to load %s", #x); } } while (0)
#define GL_OPTIONAL(x) do { _##x = proc(#x); } while (0)
   GL_REQUIRED(glFlush);
   GL_REQUIRED(glGetError);
   GL_REQUIRED(glGetIntegerv);
   GL_REQUIRED(glGetFloatv);
   GL_REQUIRED(glGetBooleanv);
   GL_REQUIRED(glGetString);
   GL_REQUIRED(glIsBuffer);
   GL_REQUIRED(glGenBuffers);
   GL_REQUIRED(glDeleteBuffers);
   GL_REQUIRED(glBindBuffer);
   GL_REQUIRED(glBufferData);
   GL_REQUIRED(glMapBufferRange);
   GL_REQUIRED(glUnmapBuffer);
   GL_REQUIRED(glPixelStorei);
   GL_REQUIRED(glReadPixels);
   GL_REQUIRED(glEnable);
   GL_REQUIRED(glDisable);
   GL_REQUIRED(glScissor);
   GL_REQUIRED(glClearColor);
   GL_REQUIRED(glClear);
   GL_OPTIONAL(glDebugMessageCallback);
#undef GL

   if (glDebugMessageCallback) {
      // GL_DEBUG_OUTPUT_SYNCHRONOUS for breakpoints (slower)
      // glEnable(GL_DEBUG_OUTPUT);
      // glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
      glDebugMessageCallback(debug_cb, NULL);
   }

   const struct { const char *p; enum gl_variant v; } variants[] = {
      { .p = "OpenGL ES-CM ", .v = OPENGL_ES },
      { .p = "OpenGL ES-CL ", .v = OPENGL_ES },
      { .p = "OpenGL ES ", .v = OPENGL_ES },
      { .p = "OpenGL ", .v = OPENGL },
   };

   const char *version = glGetString(GL_VERSION);
   WARNX("%s", version);

   for (size_t i = 0; i < ARRAY_SIZE(variants); ++i) {
      const size_t len = strlen(variants[i].p);
      if (strncmp(version, variants[i].p, len))
         continue;

      OPENGL_VARIANT = variants[i].v;
      version += len;
      break;
   }

   sscanf(version, "%u.%u", &OPENGL_VERSION.major, &OPENGL_VERSION.minor);
   loaded = true;
}
