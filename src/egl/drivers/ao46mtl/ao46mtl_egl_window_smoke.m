/* SPDX-License-Identifier: MIT */

/*
 * This is intentionally opt-in: an actual present requires a live macOS
 * WindowServer session. It uses only standard EGL plus public AppKit/Metal
 * objects, never AO46's legacy CGL or NSOpenGL compatibility interfaces.
 */

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <dlfcn.h>
#include <stdio.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

typedef void (*ao46mtl_gl_clear_color)(GLfloat red, GLfloat green,
                                       GLfloat blue, GLfloat alpha);
typedef void (*ao46mtl_gl_clear)(GLbitfield mask);
typedef GLenum (*ao46mtl_gl_get_error)(void);

static void
pump_main_run_loop(NSTimeInterval seconds)
{
   NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];

   while (deadline.timeIntervalSinceNow > 0) {
      @autoreleasepool {
         [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
      }
   }
}

static int
fail(const char *message)
{
   fprintf(stderr, "AO46 EGL window smoke: %s (EGL error 0x%04x)\n", message,
           eglGetError());
   return 1;
}

int
main(void)
{
   const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
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
   NSWindow *window = nil;
   NSView *view = nil;
   void *libgl = NULL;
   ao46mtl_gl_clear_color clear_color;
   ao46mtl_gl_clear clear;
   ao46mtl_gl_get_error get_error;
   int result = 1;

   @autoreleasepool {
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

      window = [[NSWindow alloc]
         initWithContentRect:NSMakeRect(96, 96, 96, 96)
                   styleMask:NSWindowStyleMaskTitled
                     backing:NSBackingStoreBuffered
                       defer:NO];
      view = [[NSView alloc] initWithFrame:window.contentView.bounds];
      view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      view.wantsLayer = YES;
      {
         CAMetalLayer *layer = [CAMetalLayer layer];

         layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
         if (@available(macOS 10.15, *))
            layer.allowsNextDrawableTimeout = YES;
         view.layer = layer;
      }
      window.contentView = view;
      [window makeKeyAndOrderFront:nil];
      [NSApp activateIgnoringOtherApps:NO];
      pump_main_run_loop(0.1);

      libgl = dlopen("libGL.dylib", RTLD_NOW | RTLD_LOCAL);
      if (!libgl) {
         fprintf(stderr, "AO46 EGL window smoke: dlopen(libGL.dylib): %s\n",
                 dlerror());
         goto cleanup;
      }
      clear_color = (ao46mtl_gl_clear_color)dlsym(libgl, "glClearColor");
      clear = (ao46mtl_gl_clear)dlsym(libgl, "glClear");
      get_error = (ao46mtl_gl_get_error)dlsym(libgl, "glGetError");
      if (!clear_color || !clear || !get_error) {
         fprintf(stderr, "AO46 EGL window smoke: libGL.dylib lacks GL dispatch\n");
         goto cleanup;
      }

      display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
      if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
         fail("eglInitialize failed");
         goto cleanup;
      }
      if (!eglBindAPI(EGL_OPENGL_API) ||
          !eglChooseConfig(display, config_attribs, &config, 1, &count) ||
          count != 1) {
         fail("EGL window configuration selection failed");
         goto cleanup;
      }

      surface = eglCreateWindowSurface(display, config,
                                       (EGLNativeWindowType)(__bridge void *)view,
                                       NULL);
      if (surface == EGL_NO_SURFACE) {
         EGLint error = eglGetError();

         if (error == EGL_BAD_NATIVE_WINDOW || error == EGL_BAD_SURFACE) {
            fprintf(stderr, "AO46 EGL window smoke skipped: no compositor drawable\n");
            result = 77;
         } else {
            fprintf(stderr,
                    "AO46 EGL window smoke: eglCreateWindowSurface error 0x%04x\n",
                    error);
         }
         goto cleanup;
      }

      context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
      if (context == EGL_NO_CONTEXT ||
          !eglMakeCurrent(display, surface, surface, context)) {
         fail("EGL window context creation/current failed");
         goto cleanup;
      }
      if (!eglSwapInterval(display, 1)) {
         fail("eglSwapInterval(1) failed");
         goto cleanup;
      }

      clear_color(0.125f, 0.5f, 0.875f, 1.0f);
      clear(GL_COLOR_BUFFER_BIT);
      if (get_error() != GL_NO_ERROR) {
         fprintf(stderr, "AO46 EGL window smoke: GL clear error 0x%04x\n",
                 get_error());
         goto cleanup;
      }
      if (!eglSwapBuffers(display, surface)) {
         EGLint error = eglGetError();

         if (error == EGL_BAD_SURFACE) {
            fprintf(stderr, "AO46 EGL window smoke skipped: no compositor drawable\n");
            result = 77;
         } else {
            fprintf(stderr, "AO46 EGL window smoke: eglSwapBuffers error 0x%04x\n",
                    error);
         }
         goto cleanup;
      }

      pump_main_run_loop(0.05);
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
      if (libgl)
         dlclose(libgl);
      [window orderOut:nil];
      [window close];
   }

   return result;
}
