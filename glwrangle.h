#pragma once

static GLenum (*_glGetError)(void);
static void (*_glGetIntegerv)(GLenum, GLint*);
static GLboolean (*_glIsBuffer)(GLuint);
static void (*_glGenBuffers)(GLsizei, GLuint*);
static void (*_glDeleteBuffers)(GLsizei, GLuint*);
static void (*_glBindBuffer)(GLenum, GLuint);
static void (*_glBufferData)(GLenum, GLsizeiptr, const GLvoid*, GLenum);
static void* (*_glMapBuffer)(GLenum, GLenum);
static void (*_glUnmapBuffer)(GLenum);
static void (*_glPixelStorei)(GLenum, GLint);
static void (*_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
static void (*_glPushClientAttrib)(GLbitfield);
static void (*_glPopClientAttrib)(void);
static void (*_glPushAttrib)(GLbitfield);
static void (*_glPopAttrib)(void);
static void (*_glEnable)(GLenum);
static void (*_glDisable)(GLenum);
static void (*_glScissor)(GLint, GLint, GLsizei, GLsizei);
static void (*_glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
static void (*_glClear)(GLbitfield);

#define glGetError _glGetError
#define glGetIntegerv _glGetIntegerv
#define glIsBuffer _glIsBuffer
#define glGenBuffers _glGenBuffers
#define glDeleteBuffers _glDeleteBuffers
#define glBindBuffer _glBindBuffer
#define glBufferData _glBufferData
#define glMapBuffer _glMapBuffer
#define glUnmapBuffer _glUnmapBuffer
#define glPixelStorei _glPixelStorei
#define glReadPixels _glReadPixels
#define glPushClientAttrib _glPushClientAttrib
#define glPopClientAttrib _glPopClientAttrib
#define glPushAttrib _glPushAttrib
#define glPopAttrib _glPopAttrib
#define glEnable _glEnable
#define glDisable _glDisable
#define glScissor _glScissor
#define glClearColor _glClearColor
#define glClear _glClear

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
      ERRX(EXIT_FAILURE, "There is no proc loader available");

#define GL(x) do { if (!(_##x = proc(#x))) { ERRX(EXIT_FAILURE, "Failed to load %s", #x); } } while (0)
   GL(glGetError);
   GL(glGetIntegerv);
   GL(glIsBuffer);
   GL(glGenBuffers);
   GL(glDeleteBuffers);
   GL(glBindBuffer);
   GL(glBufferData);
   GL(glMapBuffer);
   GL(glUnmapBuffer);
   GL(glPixelStorei);
   GL(glReadPixels);
   GL(glPushClientAttrib);
   GL(glPopClientAttrib);
   GL(glPushAttrib);
   GL(glPopAttrib);
   GL(glEnable);
   GL(glDisable);
   GL(glScissor);
   GL(glClearColor);
   GL(glClear);
#undef GL

   loaded = true;
}
