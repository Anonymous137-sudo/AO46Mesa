/*
 * SPDX-License-Identifier: MIT
 *
 * Built-in EGL platform for AO46's Metal Gallium screen. It supports
 * surfaceless pbuffers and public Cocoa/CAMetalLayer window presentation
 * without loading the legacy CGL or NSOpenGL compatibility runtime.
 */

#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "egllog.h"
#include "eglsurface.h"

#include "frontend/api.h"
#include "hgl_context.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "state_tracker/st_context.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "ao46mtl_screen.h"
#include "AO46MesaMetalBackend.h"

_EGL_DRIVER_STANDARD_TYPECASTS(ao46mtl_egl)

struct ao46mtl_egl_display {
   struct hgl_display *st_display;
};

struct ao46mtl_egl_context {
   _EGLContext base;
   struct st_context *st;
};

struct ao46mtl_pbuffer {
   struct pipe_frontend_drawable base;
   struct st_visual visual;
   struct pipe_screen *screen;
   struct pipe_resource *color;
   struct pipe_resource *depth_stencil;
};

struct ao46mtl_egl_surface {
   _EGLSurface base;
   struct ao46mtl_pbuffer *framebuffer;
   struct pipe_fence_handle *fence;
   void *native_window;
   struct AO46MesaMetalBackendWindow window;
   bool window_backed;
   bool window_drawable_lost;
};

static int32_t ao46mtl_next_pbuffer_id = 1;

static void
ao46mtl_visual(struct st_visual *visual)
{
   memset(visual, 0, sizeof(*visual));
   visual->buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK |
                         ST_ATTACHMENT_DEPTH_STENCIL_MASK;
   visual->color_format = PIPE_FORMAT_R8G8B8A8_UNORM;
   visual->depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;
}

static void
ao46mtl_window_visual(struct st_visual *visual,
                      enum AO46MesaMetalBackendWindowFormat format)
{
   ao46mtl_visual(visual);
   visual->color_format =
      format == AO46_MESA_METAL_BACKEND_WINDOW_BGRA8_UNORM
         ? PIPE_FORMAT_B8G8R8A8_UNORM
         : PIPE_FORMAT_R8G8B8A8_UNORM;
}

static bool
ao46mtl_pbuffer_flush_front(struct st_context *st,
                            struct pipe_frontend_drawable *drawable,
                            enum st_attachment_type attachment)
{
   (void)st;
   (void)drawable;
   (void)attachment;

   /* Pbuffers have no native front-buffer presentation target. */
   return true;
}

static bool
ao46mtl_pbuffer_validate(struct st_context *st,
                         struct pipe_frontend_drawable *drawable,
                         const enum st_attachment_type *attachments,
                         unsigned count, struct pipe_resource **out,
                         struct pipe_resource **resolve)
{
   struct ao46mtl_pbuffer *pbuffer =
      (struct ao46mtl_pbuffer *)drawable;

   (void)st;

   for (unsigned i = 0; i < count; ++i) {
      struct pipe_resource *resource = NULL;

      switch (attachments[i]) {
      case ST_ATTACHMENT_FRONT_LEFT:
      case ST_ATTACHMENT_BACK_LEFT:
         resource = pbuffer->color;
         break;
      case ST_ATTACHMENT_DEPTH_STENCIL:
         resource = pbuffer->depth_stencil;
         break;
      default:
         break;
      }

      pipe_resource_reference(&out[i], resource);
   }

   if (resolve)
      pipe_resource_reference(resolve, NULL);

   return pbuffer->color != NULL;
}

static struct ao46mtl_pbuffer *
ao46mtl_pbuffer_create(struct hgl_display *display,
                       const struct st_visual *visual,
                       unsigned width, unsigned height)
{
   struct ao46mtl_pbuffer *pbuffer = CALLOC_STRUCT(ao46mtl_pbuffer);
   struct pipe_resource template = {0};

   if (!pbuffer)
      return NULL;

   pbuffer->visual = *visual;
   pbuffer->screen = display->fscreen->screen;
   template.target = PIPE_TEXTURE_2D;
   template.format = visual->color_format;
   template.width0 = width;
   template.height0 = height;
   template.depth0 = 1;
   template.array_size = 1;
   template.last_level = 0;
   template.nr_samples = 1;
   template.nr_storage_samples = 1;
   template.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   pbuffer->color = pbuffer->screen->resource_create(pbuffer->screen, &template);

   if (visual->depth_stencil_format != PIPE_FORMAT_NONE) {
      template.format = visual->depth_stencil_format;
      template.bind = PIPE_BIND_DEPTH_STENCIL;
      pbuffer->depth_stencil =
         pbuffer->screen->resource_create(pbuffer->screen, &template);
   }

   if (!pbuffer->color ||
       (visual->depth_stencil_format != PIPE_FORMAT_NONE &&
        !pbuffer->depth_stencil)) {
      pipe_resource_reference(&pbuffer->color, NULL);
      pipe_resource_reference(&pbuffer->depth_stencil, NULL);
      FREE(pbuffer);
      return NULL;
   }

   pbuffer->base.fscreen = display->fscreen;
   pbuffer->base.visual = &pbuffer->visual;
   pbuffer->base.flush_front = ao46mtl_pbuffer_flush_front;
   pbuffer->base.validate = ao46mtl_pbuffer_validate;
   p_atomic_set(&pbuffer->base.stamp, 1);
   pbuffer->base.ID = p_atomic_inc_return(&ao46mtl_next_pbuffer_id);
   return pbuffer;
}

static void
ao46mtl_pbuffer_destroy(struct ao46mtl_pbuffer *pbuffer)
{
   if (!pbuffer)
      return;

   st_api_destroy_drawable(&pbuffer->base);
   pipe_resource_reference(&pbuffer->color, NULL);
   pipe_resource_reference(&pbuffer->depth_stencil, NULL);
   FREE(pbuffer);
}

static bool
ao46mtl_pbuffer_replace_storage(struct ao46mtl_pbuffer *pbuffer,
                                struct hgl_display *display,
                                const struct st_visual *visual,
                                unsigned width, unsigned height)
{
   struct ao46mtl_pbuffer *replacement;

   if (!pbuffer || !display || width == 0 || height == 0)
      return false;

   replacement = ao46mtl_pbuffer_create(display, visual, width, height);
   if (!replacement)
      return false;

   pipe_resource_reference(&pbuffer->color, replacement->color);
   pipe_resource_reference(&pbuffer->depth_stencil, replacement->depth_stencil);
   pbuffer->visual = replacement->visual;
   pbuffer->base.visual = &pbuffer->visual;
   p_atomic_inc(&pbuffer->base.stamp);
   ao46mtl_pbuffer_destroy(replacement);
   return true;
}

static EGLBoolean
ao46mtl_add_config(_EGLDisplay *disp)
{
   _EGLConfig *config = CALLOC_STRUCT(_egl_config);

   if (!config)
      return _eglError(EGL_BAD_ALLOC, "ao46mtl_add_config");

   _eglInitConfig(config, disp, 1);
   config->RedSize = 8;
   config->GreenSize = 8;
   config->BlueSize = 8;
   config->AlphaSize = 8;
   config->BufferSize = 32;
   config->ColorBufferType = EGL_RGB_BUFFER;
   config->ConfigCaveat = EGL_NONE;
   config->ConfigID = 1;
   config->DepthSize = 24;
   config->StencilSize = 8;
   config->Level = 0;
   config->NativeRenderable = EGL_FALSE;
   config->NativeVisualID = 0;
   config->NativeVisualType = EGL_NONE;
   config->RenderableType = EGL_OPENGL_BIT;
   config->Conformant = EGL_OPENGL_BIT;
   config->SurfaceType = EGL_PBUFFER_BIT | EGL_WINDOW_BIT;
   config->MaxPbufferWidth = 16384;
   config->MaxPbufferHeight = 16384;
   config->MaxPbufferPixels = 268435456;
   config->MinSwapInterval = 0;
   config->MaxSwapInterval = 1;

   if (!_eglValidateConfig(config, EGL_FALSE)) {
      FREE(config);
      return _eglError(EGL_BAD_CONFIG, "ao46mtl_add_config");
   }

   _eglLinkConfig(config);
   return EGL_TRUE;
}

static EGLBoolean
ao46mtl_initialize(_EGLDisplay *disp)
{
   struct ao46mtl_egl_display *ao46_display;
   struct pipe_screen *screen;

   if (disp->Platform != _EGL_PLATFORM_SURFACELESS)
      return _eglError(EGL_BAD_PARAMETER,
                       "AO46 supports EGL_PLATFORM_SURFACELESS_MESA only");

   if (disp->DriverData)
      return EGL_TRUE;

   screen = ao46mtl_screen_create();
   if (!screen)
      return _eglError(EGL_NOT_INITIALIZED,
                       "AO46 Metal Gallium screen unavailable");

   ao46_display = CALLOC_STRUCT(ao46mtl_egl_display);
   if (!ao46_display)
      return _eglError(EGL_BAD_ALLOC, "ao46mtl_initialize");

   ao46_display->st_display = hgl_create_display(screen);
   if (!ao46_display->st_display) {
      FREE(ao46_display);
      return _eglError(EGL_BAD_ALLOC, "ao46mtl state-tracker display");
   }

   disp->DriverData = ao46_display;
   disp->ClientAPIs = EGL_OPENGL_BIT;
   disp->Extensions.KHR_create_context = EGL_TRUE;
   disp->Extensions.KHR_surfaceless_context = EGL_TRUE;
   disp->Extensions.MESA_query_driver = EGL_TRUE;

   if (!ao46mtl_add_config(disp)) {
      hgl_destroy_display(ao46_display->st_display);
      disp->DriverData = NULL;
      FREE(ao46_display);
      return EGL_FALSE;
   }

   return EGL_TRUE;
}

static EGLBoolean
ao46mtl_terminate(_EGLDisplay *disp)
{
   struct ao46mtl_egl_display *ao46_display = ao46mtl_egl_display(disp);

   if (!ao46_display)
      return EGL_TRUE;

   /* The AO46 factory owns its process-wide Metal screen. EGL only retires
    * state-tracker objects here; it must not destroy a cached factory screen. */
   hgl_destroy_display(ao46_display->st_display);
   FREE(ao46_display);
   disp->DriverData = NULL;
   return EGL_TRUE;
}

static _EGLSurface *
ao46mtl_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *config,
                               const EGLint *attrib_list)
{
   struct ao46mtl_egl_display *ao46_display = ao46mtl_egl_display(disp);
   struct ao46mtl_egl_surface *surface = CALLOC_STRUCT(ao46mtl_egl_surface);
   struct st_visual visual;

   if (!surface) {
      _eglError(EGL_BAD_ALLOC, "ao46mtl_create_pbuffer_surface");
      return NULL;
   }

   if (!_eglInitSurface(&surface->base, disp, EGL_PBUFFER_BIT, config,
                        attrib_list, NULL)) {
      FREE(surface);
      return NULL;
   }

   ao46mtl_visual(&visual);
   surface->framebuffer = ao46mtl_pbuffer_create(ao46_display->st_display,
                                                   &visual, surface->base.Width,
                                                   surface->base.Height);
   if (!surface->framebuffer) {
      FREE(surface);
      _eglError(EGL_BAD_ALLOC, "ao46mtl pbuffer framebuffer");
      return NULL;
   }
   return &surface->base;
}

static void
ao46mtl_surface_wait_fence(struct ao46mtl_egl_display *ao46_display,
                           struct ao46mtl_egl_surface *surface)
{
   struct pipe_screen *screen;

   if (!ao46_display || !surface || !surface->fence)
      return;

   screen = ao46_display->st_display->fscreen->screen;
   screen->fence_finish(screen, NULL, surface->fence, OS_TIMEOUT_INFINITE);
   screen->fence_reference(screen, &surface->fence, NULL);
}

static bool
ao46mtl_prepare_window_surface(struct ao46mtl_egl_display *ao46_display,
                               struct ao46mtl_egl_surface *surface)
{
   struct AO46MesaMetalBackendWindow replacement = {0};
   struct st_visual visual;
   enum AO46MesaMetalBackendWindowFormat format;
   uint32_t width;
   uint32_t height;

   if (!surface || !surface->window_backed)
      return true;

   if (!surface->window_drawable_lost &&
       AO46MesaMetalBackendWindowIsCurrent(&surface->window))
      return true;

   ao46mtl_surface_wait_fence(ao46_display, surface);
   if (!AO46MesaMetalBackendWindowAcquire(surface->native_window, &replacement) ||
       !AO46MesaMetalBackendWindowGetInfo(&replacement, &width, &height,
                                          &format)) {
      AO46MesaMetalBackendWindowRelease(&replacement);
      return false;
   }

   ao46mtl_window_visual(&visual, format);
   if (!ao46mtl_pbuffer_replace_storage(surface->framebuffer,
                                        ao46_display->st_display, &visual,
                                        width, height)) {
      AO46MesaMetalBackendWindowRelease(&replacement);
      return false;
   }

   AO46MesaMetalBackendWindowRelease(&surface->window);
   surface->window = replacement;
   surface->base.Width = width;
   surface->base.Height = height;
   AO46MesaMetalBackendWindowSetSwapInterval(&surface->window,
                                             surface->base.SwapInterval);
   surface->window_drawable_lost = false;
   return true;
}

static _EGLSurface *
ao46mtl_create_window_surface(_EGLDisplay *disp, _EGLConfig *config,
                              void *native_window, const EGLint *attrib_list)
{
   struct ao46mtl_egl_display *ao46_display = ao46mtl_egl_display(disp);
   struct ao46mtl_egl_surface *surface = CALLOC_STRUCT(ao46mtl_egl_surface);
   struct st_visual visual;
   enum AO46MesaMetalBackendWindowFormat format;
   uint32_t width;
   uint32_t height;

   if (!surface) {
      _eglError(EGL_BAD_ALLOC, "ao46mtl_create_window_surface");
      return NULL;
   }

   if (!native_window ||
       !_eglInitSurface(&surface->base, disp, EGL_WINDOW_BIT, config,
                         attrib_list, native_window)) {
      FREE(surface);
      return NULL;
   }

   surface->native_window = native_window;
   surface->window_backed = true;
   if (!AO46MesaMetalBackendWindowAcquire(native_window, &surface->window) ||
       !AO46MesaMetalBackendWindowGetInfo(&surface->window, &width, &height,
                                          &format)) {
      AO46MesaMetalBackendWindowRelease(&surface->window);
      FREE(surface);
      _eglError(EGL_BAD_NATIVE_WINDOW,
                "AO46 requires an sRGB CAMetalLayer, NSView, or NSWindow");
      return NULL;
   }

   ao46mtl_window_visual(&visual, format);
   surface->framebuffer = ao46mtl_pbuffer_create(ao46_display->st_display,
                                                   &visual, width, height);
   if (!surface->framebuffer) {
      AO46MesaMetalBackendWindowRelease(&surface->window);
      FREE(surface);
      _eglError(EGL_BAD_ALLOC, "ao46mtl window framebuffer");
      return NULL;
   }

   surface->base.Width = width;
   surface->base.Height = height;
   AO46MesaMetalBackendWindowSetSwapInterval(&surface->window,
                                             surface->base.SwapInterval);
   return &surface->base;
}

static EGLBoolean
ao46mtl_destroy_surface(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct ao46mtl_egl_display *ao46_display = ao46mtl_egl_display(disp);

   if (_eglPutSurface(surface)) {
      struct ao46mtl_egl_surface *ao46_surface = ao46mtl_egl_surface(surface);
      struct pipe_screen *screen = ao46_display->st_display->fscreen->screen;

      screen->fence_reference(screen, &ao46_surface->fence, NULL);
      AO46MesaMetalBackendWindowRelease(&ao46_surface->window);
      ao46mtl_pbuffer_destroy(ao46_surface->framebuffer);
      FREE(ao46_surface);
   }

   return EGL_TRUE;
}

static _EGLContext *
ao46mtl_create_context(_EGLDisplay *disp, _EGLConfig *config,
                       _EGLContext *share_list, const EGLint *attrib_list)
{
   struct ao46mtl_egl_display *ao46_display = ao46mtl_egl_display(disp);
   struct ao46mtl_egl_context *context = CALLOC_STRUCT(ao46mtl_egl_context);
   struct st_context_attribs attributes;
   enum st_context_error error;
   struct st_visual visual;

   if (!context) {
      _eglError(EGL_BAD_ALLOC, "ao46mtl_create_context");
      return NULL;
   }

   if (!_eglInitContext(&context->base, disp, config, share_list, attrib_list)) {
      FREE(context);
      return NULL;
   }

   if (context->base.ClientAPI != EGL_OPENGL_API) {
      FREE(context);
      _eglError(EGL_BAD_MATCH, "AO46 only exposes desktop OpenGL through EGL");
      return NULL;
   }

   memset(&attributes, 0, sizeof(attributes));
   ao46mtl_visual(&visual);
   attributes.visual = visual;
   attributes.profile =
      context->base.Profile == EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR
         ? API_OPENGL_CORE
         : API_OPENGL_COMPAT;
   attributes.major = context->base.ClientMajorVersion;
   attributes.minor = context->base.ClientMinorVersion;
   attributes.options.force_glsl_extensions_warn = false;

   if (context->base.Flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR)
      attributes.flags |= ST_CONTEXT_FLAG_DEBUG;
   if (context->base.Flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR)
      attributes.flags |= ST_CONTEXT_FLAG_FORWARD_COMPATIBLE;
   if (context->base.NoError)
      attributes.flags |= ST_CONTEXT_FLAG_NO_ERROR;

   context->st = st_api_create_context(
      ao46_display->st_display->fscreen, &attributes, &error,
      share_list ? ao46mtl_egl_context(share_list)->st : NULL);
   if (!context->st) {
      FREE(context);
      _eglError(EGL_BAD_MATCH, "AO46 cannot satisfy requested OpenGL profile");
      return NULL;
   }

   context->st->frontend_context = context;
   return &context->base;
}

static EGLBoolean
ao46mtl_destroy_context(_EGLDisplay *disp, _EGLContext *context)
{
   (void)disp;

   if (_eglPutContext(context)) {
      struct ao46mtl_egl_context *ao46_context = ao46mtl_egl_context(context);

      st_context_flush(ao46_context->st, 0, NULL, NULL, NULL);
      st_destroy_context(ao46_context->st);
      FREE(ao46_context);
   }

   return EGL_TRUE;
}

static EGLBoolean
ao46mtl_make_current(_EGLDisplay *disp, _EGLSurface *draw,
                     _EGLSurface *read, _EGLContext *context)
{
   _EGLContext *old_context;
   _EGLSurface *old_draw;
   _EGLSurface *old_read;
   struct ao46mtl_egl_context *ao46_context = ao46mtl_egl_context(context);
   struct ao46mtl_egl_surface *ao46_draw = ao46mtl_egl_surface(draw);
   struct ao46mtl_egl_surface *ao46_read = ao46mtl_egl_surface(read);

   if ((ao46_draw && !ao46mtl_prepare_window_surface(ao46mtl_egl_display(disp),
                                                      ao46_draw)) ||
       (ao46_read && ao46_read != ao46_draw &&
        !ao46mtl_prepare_window_surface(ao46mtl_egl_display(disp), ao46_read)))
      return _eglError(EGL_BAD_SURFACE,
                       "AO46 could not refresh the Cocoa drawable");

   if (!_eglBindContext(context, draw, read, &old_context, &old_draw, &old_read))
      return EGL_FALSE;

   if (old_context == context && old_draw == draw && old_read == read) {
      _eglPutSurface(old_draw);
      _eglPutSurface(old_read);
      _eglPutContext(old_context);
      return EGL_TRUE;
   }

   if (!st_api_make_current(ao46_context ? ao46_context->st : NULL,
                            ao46_draw ? &ao46_draw->framebuffer->base : NULL,
                            ao46_read ? &ao46_read->framebuffer->base : NULL))
      return _eglError(EGL_BAD_ACCESS, "AO46 state-tracker make-current failed");

   if (old_draw)
      ao46mtl_destroy_surface(disp, old_draw);
   if (old_read)
      ao46mtl_destroy_surface(disp, old_read);
   if (old_context)
      ao46mtl_destroy_context(disp, old_context);

   return EGL_TRUE;
}

static EGLBoolean
ao46mtl_swap_buffers(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct ao46mtl_egl_display *ao46_display = ao46mtl_egl_display(disp);
   struct ao46mtl_egl_surface *ao46_surface = ao46mtl_egl_surface(surface);
   struct ao46mtl_egl_context *ao46_context =
      ao46mtl_egl_context(surface->CurrentContext);
   struct pipe_fence_handle *new_fence = NULL;

   if (!ao46_context)
      return _eglError(EGL_BAD_CURRENT_SURFACE, "AO46 pbuffer is not current");

   st_context_flush(ao46_context->st, ST_FLUSH_FRONT, &new_fence, NULL, NULL);
   ao46mtl_surface_wait_fence(ao46_display, ao46_surface);
   ao46_surface->fence = new_fence;

   if (ao46_surface->window_backed &&
       !AO46MesaMetalBackendWindowPresent(&ao46_surface->window,
                                          ao46_context->st->pipe,
                                          ao46_surface->framebuffer->color)) {
      ao46_surface->window_drawable_lost = true;
      return _eglError(EGL_BAD_SURFACE,
                       "AO46 could not acquire or present a Cocoa drawable");
   }

   return EGL_TRUE;
}

static EGLBoolean
ao46mtl_swap_interval(_EGLDisplay *disp, _EGLSurface *surface,
                      EGLint interval)
{
   (void)disp;
   struct ao46mtl_egl_surface *ao46_surface = ao46mtl_egl_surface(surface);

   if (!ao46_surface || interval < 0 || interval > 1)
      return _eglError(EGL_BAD_PARAMETER,
                       "AO46 supports EGL swap intervals 0 and 1 only");

   surface->SwapInterval = interval;
   if (ao46_surface->window_backed)
      AO46MesaMetalBackendWindowSetSwapInterval(&ao46_surface->window,
                                                (unsigned)interval);
   return EGL_TRUE;
}

const _EGLDriver _eglDriver = {
   .Initialize = ao46mtl_initialize,
   .Terminate = ao46mtl_terminate,
   .CreateContext = ao46mtl_create_context,
   .DestroyContext = ao46mtl_destroy_context,
   .MakeCurrent = ao46mtl_make_current,
   .CreateWindowSurface = ao46mtl_create_window_surface,
   .CreatePbufferSurface = ao46mtl_create_pbuffer_surface,
   .DestroySurface = ao46mtl_destroy_surface,
   .SwapInterval = ao46mtl_swap_interval,
   .SwapBuffers = ao46mtl_swap_buffers,
};
