#pragma once

static void* (*_dlsym)(void*, const char*) = NULL;
static EGLBoolean (*_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
static __eglMustCastToProperFunctionPointerType (*_eglGetProcAddress)(const char*) = NULL;
static void (*_glXSwapBuffers)(Display*, GLXDrawable) = NULL;
static __GLXextFuncPtr (*_glXGetProcAddress)(const GLubyte*) = NULL;
static __GLXextFuncPtr (*_glXGetProcAddressARB)(const GLubyte*) = NULL;
static snd_pcm_sframes_t (*_snd_pcm_writei)(snd_pcm_t*, const void*, snd_pcm_uframes_t) = NULL;
static snd_pcm_sframes_t (*_snd_pcm_writen)(snd_pcm_t*, void**, snd_pcm_uframes_t) = NULL;
static snd_pcm_sframes_t (*_snd_pcm_mmap_writei)(snd_pcm_t*, const void*, snd_pcm_uframes_t) = NULL;
static snd_pcm_sframes_t (*_snd_pcm_mmap_writen)(snd_pcm_t*, void**, snd_pcm_uframes_t) = NULL;
static int (*_clock_gettime)(clockid_t, struct timespec*) = NULL;
static void* store_real_symbol_and_return_fake_symbol(const char*, void*);
static void hook_function(void**, const char*, const bool);

#define HOOK(x) hook_function((void**)&_##x, #x, false)

EGLBoolean
eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
   HOOK(eglSwapBuffers);
   swap_buffers();
   return _eglSwapBuffers(dpy, surface);
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *procname)
{
   HOOK(eglGetProcAddress);
   return (_eglGetProcAddress ? store_real_symbol_and_return_fake_symbol(procname, _eglGetProcAddress(procname)) : NULL);
}

void
glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
   HOOK(glXSwapBuffers);
   swap_buffers();
   _glXSwapBuffers(dpy, drawable);
}

__GLXextFuncPtr
glXGetProcAddressARB(const GLubyte *procname)
{
   HOOK(glXGetProcAddressARB);
   return (_glXGetProcAddressARB ? store_real_symbol_and_return_fake_symbol((const char*)procname, _glXGetProcAddressARB(procname)) : NULL);
}

__GLXextFuncPtr
glXGetProcAddress(const GLubyte *procname)
{
   HOOK(glXGetProcAddress);
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

   if ((clk_id == CLOCK_MONOTONIC || clk_id == CLOCK_MONOTONIC_RAW)) {
      const uint64_t fake = get_fake_time_ns();
      tp->tv_sec = fake / (uint64_t)1e9;
      tp->tv_nsec = (fake % (uint64_t)1e9);
      return 0;
   }

   return _clock_gettime(clk_id, tp);
}

static void*
store_real_symbol_and_return_fake_symbol(const char *symbol, void *ret)
{
   if (!ret || !symbol)
      return ret;

   if (0) {}
#define SET_IF_NOT_HOOKED(x, y) do { if (!_##x) { _##x = y; WARNX("SET %s to %p", #x, y); } } while (0)
#define FAKE_SYMBOL(x) else if (!strcmp(symbol, #x)) { SET_IF_NOT_HOOKED(x, ret); return x; }
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

#define HOOK_DLSYM(x) hook_function((void**)&_##x, #x, true)

static void
hook_function(void **ptr, const char *name, const bool versioned)
{
   if (*ptr)
      return;

   if (versioned) {
      const char *versions[] = { "GLIBC_2.0", "GLIBC_2.2.5", NULL };
      for (size_t i = 0; !*ptr && versions[i]; ++i)
         *ptr = dlvsym(RTLD_NEXT, name, versions[i]);
   } else {
      HOOK_DLSYM(dlsym);
      *ptr = _dlsym(RTLD_NEXT, name);
   }

   if (!*ptr)
      ERRX(EXIT_FAILURE, "HOOK FAIL %s", name);

   WARNX("HOOK %s", name);
}

void*
dlsym(void *handle, const char *symbol)
{
   HOOK_DLSYM(dlsym);

   if (!strcmp(symbol, "dlsym"))
      return dlsym;

   return store_real_symbol_and_return_fake_symbol(symbol, _dlsym(handle, symbol));
}

#undef HOOK_DLSYM
