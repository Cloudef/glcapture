#pragma once

static void* (*_dlsym)(void*, const char*);
static void (*_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static EGLBoolean (*_eglSwapBuffers)(EGLDisplay, EGLSurface);
static __eglMustCastToProperFunctionPointerType (*_eglGetProcAddress)(const char*);
static void (*_glXSwapBuffers)(Display*, GLXDrawable);
static __GLXextFuncPtr (*_glXGetProcAddress)(const GLubyte*);
static __GLXextFuncPtr (*_glXGetProcAddressARB)(const GLubyte*);
static snd_pcm_sframes_t (*_snd_pcm_writei)(snd_pcm_t*, const void*, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*_snd_pcm_writen)(snd_pcm_t*, void**, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*_snd_pcm_mmap_writei)(snd_pcm_t*, const void*, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*_snd_pcm_mmap_writen)(snd_pcm_t*, void**, snd_pcm_uframes_t);
static int (*_clock_gettime)(clockid_t, struct timespec*);
static void* store_real_symbol_and_return_fake_symbol(const char*, void*);
static void hook_function(void**, const char*, const bool, const char*[]);
static void hook_dlsym(void**, const char*);

#define HOOK(x) hook_function((void**)&_##x, #x, false, NULL)
#define HOOK_FROM(x, ...) hook_function((void**)&_##x, #x, false, (const char*[]){ __VA_ARGS__, NULL })

// Use HOOK_FROM with this list for any GL/GLX stuff
#define GL_LIBS "libGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLX.so"

void
glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)
{
   HOOK_FROM(glBlitFramebuffer, GL_LIBS);
   const GLint a[] = { srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1 };
   memcpy(LAST_FRAMEBUFFER_BLIT, a, sizeof(a));
   _glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

EGLBoolean
eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
   HOOK_FROM(eglSwapBuffers, "libEGL.so");
   swap_buffers();
   return _eglSwapBuffers(dpy, surface);
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *procname)
{
   HOOK_FROM(eglGetProcAddress, "libEGL.so");
   return (_eglGetProcAddress ? store_real_symbol_and_return_fake_symbol(procname, _eglGetProcAddress(procname)) : NULL);
}

void
glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
   HOOK_FROM(glXSwapBuffers, GL_LIBS);
   swap_buffers();
   _glXSwapBuffers(dpy, drawable);
}

__GLXextFuncPtr
glXGetProcAddressARB(const GLubyte *procname)
{
   HOOK_FROM(glXGetProcAddressARB, GL_LIBS);
   return (_glXGetProcAddressARB ? store_real_symbol_and_return_fake_symbol((const char*)procname, _glXGetProcAddressARB(procname)) : NULL);
}

__GLXextFuncPtr
glXGetProcAddress(const GLubyte *procname)
{
   HOOK_FROM(glXGetProcAddress, GL_LIBS);
   return (_glXGetProcAddress ? store_real_symbol_and_return_fake_symbol((const char*)procname, _glXGetProcAddress(procname)) : NULL);
}

snd_pcm_sframes_t
snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
   HOOK(snd_pcm_writei);
   alsa_writei(pcm, buffer, size, __func__);
   return _snd_pcm_writei(pcm, buffer, size);
}

snd_pcm_sframes_t
snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
   HOOK(snd_pcm_writen);
   // FIXME: Implement
   return _snd_pcm_writen(pcm, bufs, size);
}

snd_pcm_sframes_t
snd_pcm_mmap_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
   HOOK(snd_pcm_mmap_writei);
   alsa_writei(pcm, buffer, size, __func__);
   return _snd_pcm_mmap_writei(pcm, buffer, size);
}

snd_pcm_sframes_t
snd_pcm_mmap_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
   HOOK(snd_pcm_mmap_writen);
   // FIXME: Implement
   return _snd_pcm_mmap_writen(pcm, bufs, size);
}

int
clock_gettime(clockid_t clk_id, struct timespec *tp)
{
   HOOK(clock_gettime);
   const uint64_t fake = get_fake_time_ns(clk_id);
   tp->tv_sec = fake / (uint64_t)1e9;
   tp->tv_nsec = (fake % (uint64_t)1e9);
   return 0;
}

static void*
store_real_symbol_and_return_fake_symbol(const char *symbol, void *ret)
{
   if (!ret || !symbol)
      return ret;

   if (0) {}
#define SET_IF_NOT_HOOKED(x, y) do { if (!_##x) { _##x = y; WARNX("SET %s to %p", #x, y); } } while (0)
#define FAKE_SYMBOL(x) else if (!strcmp(symbol, #x)) { SET_IF_NOT_HOOKED(x, ret); return x; }
   FAKE_SYMBOL(glBlitFramebuffer)
   FAKE_SYMBOL(eglSwapBuffers)
   FAKE_SYMBOL(eglGetProcAddress)
   FAKE_SYMBOL(glXSwapBuffers)
   FAKE_SYMBOL(glXGetProcAddressARB)
   FAKE_SYMBOL(glXGetProcAddress)
   FAKE_SYMBOL(snd_pcm_writei)
   FAKE_SYMBOL(snd_pcm_writen)
   FAKE_SYMBOL(snd_pcm_mmap_writei)
   FAKE_SYMBOL(snd_pcm_mmap_writen)
   FAKE_SYMBOL(clock_gettime)
#undef FAKE_SYMBOL
#undef SET_IF_NOT_HOOKED

   return ret;
}

#define HOOK_DLSYM(x) hook_dlsym((void**)&_##x, #x)

static void*
get_symbol(void *src, const char *name, const bool versioned)
{
   if (!src)
      return NULL;

   if (versioned) {
      void *ptr = NULL;
      const char *versions[] = { "GLIBC_2.0", "GLIBC_2.2.5" };
      for (size_t i = 0; !ptr && i < ARRAY_SIZE(versions); ++i)
         ptr = dlvsym(src, name, versions[i]);
      return ptr;
   }

   HOOK_DLSYM(dlsym);
   return _dlsym(src, name);
}

static void
hook_function(void **ptr, const char *name, const bool versioned, const char *srcs[])
{
   if (*ptr)
      return;

   *ptr = get_symbol(RTLD_NEXT, name, versioned);

   for (size_t i = 0; !*ptr && srcs && srcs[i]; ++i) {
      // If we know where the symbol comes from, but program e.g. used dlopen with RTLD_LOCAL
      // Should be only needed with GL/GLES/EGL stuff as we don't link to those for reason.
      void *so = dlopen(srcs[i], RTLD_LAZY | RTLD_NOLOAD);
      WARNX("Trying dlopen: %s (%p) (RTLD_LAZY | RTLD_NOLOAD)", srcs[i], so);
      *ptr = get_symbol(so, name, versioned);
   }

   if (!*ptr)
      ERRX(EXIT_FAILURE, "HOOK FAIL %s", name);

   WARNX("HOOK %s", name);
}

static void
hook_dlsym(void **ptr, const char *name)
{
   if (*ptr)
      return;

   hook_function(ptr, name, true, NULL);

   void *next;
   if ((next = _dlsym(RTLD_NEXT, name))) {
      WARNX("chaining %s: %p -> %p", name, ptr, next);
      *ptr = next;
   }
}

void*
dlsym(void *handle, const char *symbol)
{
   HOOK_DLSYM(dlsym);

   if (!strcmp(symbol, "dlsym"))
      return dlsym;

   return store_real_symbol_and_return_fake_symbol(symbol, _dlsym(handle, symbol));
}
