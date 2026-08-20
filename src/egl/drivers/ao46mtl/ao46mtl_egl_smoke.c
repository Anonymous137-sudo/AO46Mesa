/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <stdio.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

typedef void (*ao46mtl_gl_clear_color)(GLfloat red, GLfloat green,
                                       GLfloat blue, GLfloat alpha);
typedef void (*ao46mtl_gl_clear)(GLbitfield mask);
typedef void (*ao46mtl_gl_finish)(void);
typedef void (*ao46mtl_gl_read_pixels)(GLint x, GLint y, GLsizei width,
                                        GLsizei height, GLenum format,
                                        GLenum type, void *pixels);
typedef GLenum (*ao46mtl_gl_get_error)(void);
typedef const GLubyte *(*ao46mtl_gl_get_string)(GLenum name);

static int
fail(const char *message)
{
   fprintf(stderr, "AO46 EGL smoke: %s (EGL error 0x%04x)\n", message,
           eglGetError());
   return 1;
}

int
main(void)
{
   const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   const EGLint pbuffer_attribs[] = {
      EGL_WIDTH, 4,
      EGL_HEIGHT, 4,
      EGL_NONE,
   };
   const EGLint context_attribs[] = {
      EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
      EGL_CONTEXT_MINOR_VERSION_KHR, 3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
      EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
      EGL_NONE,
   };
   EGLConfig config;
   EGLContext context = EGL_NO_CONTEXT;
   EGLDisplay display = EGL_NO_DISPLAY;
   EGLSurface surface = EGL_NO_SURFACE;
   EGLint count = 0;
   void *libgl;
   ao46mtl_gl_clear_color clear_color;
   ao46mtl_gl_clear clear;
   ao46mtl_gl_finish finish;
   ao46mtl_gl_read_pixels read_pixels;
   ao46mtl_gl_get_error get_error;
   ao46mtl_gl_get_string get_string;
   unsigned char pixel[4] = {0, 0, 0, 0};
   int result = 1;

   libgl = dlopen("libGL.dylib", RTLD_NOW | RTLD_LOCAL);
   if (!libgl) {
      fprintf(stderr, "AO46 EGL smoke: unable to load libGL.dylib: %s\n",
              dlerror());
      return 1;
   }

   clear_color = (ao46mtl_gl_clear_color)dlsym(libgl, "glClearColor");
   clear = (ao46mtl_gl_clear)dlsym(libgl, "glClear");
   finish = (ao46mtl_gl_finish)dlsym(libgl, "glFinish");
   read_pixels = (ao46mtl_gl_read_pixels)dlsym(libgl, "glReadPixels");
   get_error = (ao46mtl_gl_get_error)dlsym(libgl, "glGetError");
   get_string = (ao46mtl_gl_get_string)dlsym(libgl, "glGetString");
   if (!clear_color || !clear || !finish || !read_pixels || !get_error ||
       !get_string) {
      fprintf(stderr, "AO46 EGL smoke: libGL.dylib lacks Mesa GL entry points\n");
      goto cleanup;
   }

   /* A surfaceless display has no native-display handle. */
   display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
      fail("eglInitialize failed");
      goto cleanup;
   }

   if (!eglBindAPI(EGL_OPENGL_API) ||
       !eglChooseConfig(display, config_attribs, &config, 1, &count) ||
       count != 1) {
      fail("EGL configuration selection failed");
      goto cleanup;
   }

   surface = eglCreatePbufferSurface(display, config, pbuffer_attribs);
   if (surface == EGL_NO_SURFACE) {
      fail("eglCreatePbufferSurface failed");
      goto cleanup;
   }

   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
   if (context == EGL_NO_CONTEXT) {
      fail("eglCreateContext failed");
      goto cleanup;
   }

   if (!eglMakeCurrent(display, surface, surface, context)) {
      fail("eglMakeCurrent failed");
      goto cleanup;
   }

   if (!get_string(GL_VERSION) || get_error() != GL_NO_ERROR) {
      fprintf(stderr, "AO46 EGL smoke: GL dispatch is not current (GL error 0x%04x)\n",
              get_error());
      goto cleanup;
   }

   clear_color(0.125f, 0.5f, 0.875f, 1.0f);
   clear(GL_COLOR_BUFFER_BIT);
   finish();
   if (get_error() != GL_NO_ERROR) {
      fprintf(stderr, "AO46 EGL smoke: clear/finish failed (GL error 0x%04x)\n",
              get_error());
      goto cleanup;
   }
   read_pixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   if (get_error() != GL_NO_ERROR) {
      fprintf(stderr, "AO46 EGL smoke: readback failed (GL error 0x%04x)\n",
              get_error());
      goto cleanup;
   }
   if (pixel[0] < 30 || pixel[0] > 35 ||
       pixel[1] < 126 || pixel[1] > 129 ||
       pixel[2] < 221 || pixel[2] > 225 || pixel[3] != 255) {
      fprintf(stderr, "AO46 EGL smoke: unexpected RGBA %u %u %u %u\n",
              pixel[0], pixel[1], pixel[2], pixel[3]);
      goto cleanup;
   }

   if (!eglSwapBuffers(display, surface)) {
      fail("eglSwapBuffers failed");
      goto cleanup;
   }

   result = 0;

cleanup:
   if (display != EGL_NO_DISPLAY) {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context != EGL_NO_CONTEXT)
         eglDestroyContext(display, context);
      if (surface != EGL_NO_SURFACE)
         eglDestroySurface(display, surface);
      eglTerminate(display);
   }
   dlclose(libgl);
   return result;
}
