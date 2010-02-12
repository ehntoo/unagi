/*
 * Copyright (C) 2009 Arnaud "arnau" Fontaine <arnau@mini-dweeb.org>
 *
 * This  program is  free  software: you  can  redistribute it  and/or
 * modify  it under the  terms of  the GNU  General Public  License as
 * published by the Free Software  Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT  ANY  WARRANTY;  without   even  the  implied  warranty  of
 * MERCHANTABILITY or  FITNESS FOR A PARTICULAR PURPOSE.   See the GNU
 * General Public License for more details.
 *
 * You should have  received a copy of the  GNU General Public License
 *  along      with      this      program.      If      not,      see
 *  <http://www.gnu.org/licenses/>.
 */

/** \file
 *  \brief Rendering backend based on X Render extension
 */

#include <stdlib.h>
#include <assert.h>

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>

#include "window.h"
#include "structs.h"
#include "plugin.h"
#include "util.h"

/** Information related to Render */
typedef struct
{
  /** Extension information */
  const xcb_query_extension_reply_t *ext;
  /** Picture associated with the root window */
  xcb_render_picture_t picture;
  /** Buffer Picture used to paint the windows before the root Picture */
  xcb_render_picture_t buffer_picture;
  /** Picture associated with the background Pixmap */
  xcb_render_picture_t background_picture;
  /** All Picture formats supported by the screen */
  xcb_render_query_pict_formats_reply_t *pict_formats;
  /** Picture Visual supported by the screen */
  xcb_render_pictvisual_t *pictvisual;
} _render_conf_t;

static _render_conf_t _render_conf;

/** Information related to Render specific to windows */
typedef struct
{
  /** Picture associated with the Window Pixmap */
  xcb_render_picture_t picture;
  /** Alpha Picture of the Window */ 
  xcb_render_picture_t alpha_picture;
} _render_window_t;

/** Request label of Render extension for X error reporting, which are
 *  uniquely identified according to  their minor opcode starting from
 *  0 */
static const char *_render_request_label[] = {
  "RenderQueryVersion",
  "RenderQueryPictFormats",
  "RenderQueryPictIndexValues",
  "Render minor 3",
  "RenderCreatePicture",
  "RenderChangePicture",
  "RenderSetPictureClipRectangles",
  "RenderFreePicture",
  "RenderComposite",
  "Render minor 9",
  "RenderTrapezoids",
  "RenderTriangles",
  "RenderTriStrip",
  "RenderTriFan",
  "Render minor 14",
  "Render minor 15",
  "Render minor 16",
  "RenderCreateGlyphSet",
  "RenderReferenceGlyphSet",
  "RenderFreeGlyphSet",
  "RenderAddGlyphs",
  "Render minor 21",
  "RenderFreeGlyphs",
  "RenderCompositeGlyphs8",
  "RenderCompositeGlyphs16",
  "RenderCompositeGlyphs32",
  "RenderFillRectangles",
  "RenderCreateCursor",
  "RenderSetPictureTransform",
  "RenderQueryFilters",
  "RenderSetPictureFilter",
  "RenderCreateAnimCursor",
  "RenderAddTraps",
  "RenderCreateSolidFill",
  "RenderCreateLinearGradient",
  "RenderCreateRadialGradient",
  "RenderCreateConicalGradient"
};

/** Error label of X Render extension for X error reporting, which are
 *  uniquely identified by the first  error of the extension (as given
 *  in the extension information) added to the error code value
 */
static const char *_render_error_label[] = {
  "PictFormat",
  "Picture",
  "PictOp",
  "GlyphSet",
  "Glyph"
};

/** Cookie request used on backend initialisation (not thread-safe but
    we don't mind for initialisation) */
static xcb_render_query_version_cookie_t _render_version_cookie = { 0 };
static xcb_render_query_pict_formats_cookie_t _render_pict_formats_cookie = { 0 };

/** Called on dlopen() and only prefetch the Render extension data */
static void __attribute__((constructor))
render_preinit(void)
{
  xcb_prefetch_extension_data(globalconf.connection, &xcb_render_id);
}

/** Check whether  the Render extension  is present and  send requests
 *  (such as QueryVersion and RenderQueryPictFormats)
 *
 * \return True if the Render extension is present
 */
static bool
render_init(void)
{
  _render_conf.ext = xcb_get_extension_data(globalconf.connection,
					    &xcb_render_id);

  if(!_render_conf.ext || !_render_conf.ext->present)
    {
      fatal("No render extension");
      return false;
    }

  _render_version_cookie =
    xcb_render_query_version_unchecked(globalconf.connection,
				       XCB_RENDER_MAJOR_VERSION,
				       XCB_RENDER_MINOR_VERSION);

  _render_pict_formats_cookie =
    xcb_render_query_pict_formats_unchecked(globalconf.connection);

  /* Send requests to get the root window background pixmap */ 
  window_get_root_background_pixmap();

  return true;
}

/** Fill the  root background with a  color as there  is no background
 *  image available
 */
static void
_render_root_background_fill(void)
{
  const xcb_rectangle_t root_rectangle = {
    .x = 0, .y = 0, .width = globalconf.screen->width_in_pixels,
    .height = globalconf.screen->height_in_pixels
  };

  const xcb_render_color_t root_color = {
    .red = 0x8080, .green = 0x8080, .blue = 0x8080,
    .alpha = 0xffff
  };

  xcb_render_fill_rectangles(globalconf.connection, XCB_RENDER_PICT_OP_SRC,
			     _render_conf.background_picture, root_color, 1,
			     &root_rectangle);
}

/** Paint the buffer Picture to the root Picture */
static inline void
_render_paint_root_buffer_to_root(void)
{
  xcb_render_composite(globalconf.connection,
		       XCB_RENDER_PICT_OP_SRC,
		       _render_conf.buffer_picture, XCB_NONE, _render_conf.picture,
		       0, 0, 0, 0, 0, 0,
		       globalconf.screen->width_in_pixels,
		       globalconf.screen->height_in_pixels);
}

/** Paint the background to the buffer Picture */
static inline void
_render_paint_root_background_to_buffer(void)
{
  xcb_render_composite(globalconf.connection, XCB_RENDER_PICT_OP_SRC,
		       _render_conf.background_picture, XCB_NONE,
		       _render_conf.buffer_picture, 0, 0, 0, 0, 0, 0,
		       globalconf.screen->width_in_pixels,
		       globalconf.screen->height_in_pixels);
}

/** Create the root background  Picture associated with the background
 *  image Pixmap  (as given by _XROOTPMAP_ID or  _XSETROOT_ID) if any,
 *  otherwise, fill the background with a color
 */
static void
_render_init_root_background(void)
{
  /* Get the background image pixmap, if any, otherwise do nothing */
  xcb_pixmap_t root_background_pixmap = window_get_root_background_pixmap_finalise();
  bool root_background_fill = false;

  if(!root_background_pixmap)
    {
      debug("No background pixmap set, set default background color");
      root_background_pixmap = window_new_root_background_pixmap();
      root_background_fill = true;
    }

  _render_conf.background_picture = xcb_generate_id(globalconf.connection);
  const uint32_t root_buffer_val = true;

  /* Create a new picture holding the background pixmap */
  xcb_render_create_picture(globalconf.connection,
			    _render_conf.background_picture,
			    root_background_pixmap,
			    _render_conf.pictvisual->format,
			    XCB_RENDER_CP_REPEAT, &root_buffer_val);

  if(root_background_fill)
    {
      xcb_free_pixmap(globalconf.connection, root_background_pixmap);
      _render_root_background_fill();
    }
}

/** Create the Picture associated with the root Window, its background
 *  and paint it to the root Window
 *
 */
static bool
_render_init_root_picture(void)
{
  /* Now  create the  root window  picture used  when  compositing but
     before get the screen visuals... */

  assert(_render_pict_formats_cookie.sequence);

  /* The  "PictFormat" object  holds information  needed  to translate
     pixel values into red, green, blue and alpha channels */
  _render_conf.pict_formats =
    xcb_render_query_pict_formats_reply(globalconf.connection,
					_render_pict_formats_cookie,
					NULL);

  if(!_render_conf.pict_formats ||
     !xcb_render_query_pict_formats_formats_length(_render_conf.pict_formats) ||
     !(_render_conf.pictvisual = xcb_render_util_find_visual_format(_render_conf.pict_formats,
								    globalconf.screen->root_visual)))
    {
      free(_render_conf.pict_formats);

      fatal("Can't get PictFormat of root window");
      return false;
    }

  /* Create Picture associated with the root window */
  {
    _render_conf.picture = xcb_generate_id(globalconf.connection);
    const uint32_t root_picture_val = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;

    xcb_render_create_picture(globalconf.connection,
			      _render_conf.picture,
			      globalconf.screen->root,
			      _render_conf.pictvisual->format,
			      XCB_RENDER_CP_SUBWINDOW_MODE,
			      &root_picture_val);
  }

  /* Create a buffer Picture to  avoid image flickering when trying to
     draw on the root window Picture directly */
  {
    xcb_pixmap_t pixmap = xcb_generate_id(globalconf.connection);
    
    xcb_create_pixmap(globalconf.connection, globalconf.screen->root_depth, pixmap,
		      globalconf.screen->root, globalconf.screen->width_in_pixels,
		      globalconf.screen->height_in_pixels);

    _render_conf.buffer_picture = xcb_generate_id(globalconf.connection);

    xcb_render_create_picture(globalconf.connection,
			      _render_conf.buffer_picture,
			      pixmap,
			      _render_conf.pictvisual->format,
			      0, NULL);

    xcb_free_pixmap(globalconf.connection, pixmap);
  }

  /* Initialise the root  background Picture and paint it  to the root
     Picture buffer for now */
  _render_init_root_background();
  _render_paint_root_background_to_buffer();
  _render_paint_root_buffer_to_root();

  return true;
}

/** Last step of rendering backend initialisation */
static bool
render_init_finalise(void)
{
  assert(_render_version_cookie.sequence);

  xcb_render_query_version_reply_t *render_version_reply =
    xcb_render_query_version_reply(globalconf.connection,
				   _render_version_cookie,
				   NULL);

  /* Alpha support needed */
  if(!render_version_reply || render_version_reply->minor_version < 1)
    {
      free(render_version_reply);

      fatal("Need Render extension 0.1 at least");
      return false;
    }

  free(render_version_reply);

  return _render_init_root_picture();
}

/** Reset the background,  used in case the root  window is resized or
 *  the root background image has changed
 */
static void
render_reset_background(void)
{
  xcb_render_free_picture(globalconf.connection,
			  _render_conf.background_picture);

  /* Send requests to get the root window background pixmap */ 
  window_get_root_background_pixmap();

  _render_init_root_background();
}

/** Create the alpha Picture associated  with a window by only filling
 *  it with the alpha channel value
 *
 * \param alpha_picture The alpha Picture of the Window
 * \param opacity The Window opacity
 */
static void
_render_create_window_alpha_picture(xcb_render_picture_t *alpha_picture,
				    const uint16_t opacity)
{
  const xcb_pixmap_t pixmap = xcb_generate_id(globalconf.connection);

  xcb_create_pixmap(globalconf.connection, 8, pixmap,
		    globalconf.screen->root, 1, 1);

  const uint32_t create_picture_val = true;

  *alpha_picture = xcb_generate_id(globalconf.connection);

  xcb_render_create_picture(globalconf.connection,
			    *alpha_picture,
			    pixmap,
			    xcb_render_util_find_standard_format(_render_conf.pict_formats,
								 XCB_PICT_STANDARD_A_8)->id,
			    XCB_RENDER_CP_REPEAT,
			    &create_picture_val);

  const xcb_render_color_t color = {
    .red = 0, .green = 0, .blue = 0,
    .alpha = opacity
  };

  const xcb_rectangle_t rect = { .x = 0, .y = 0, .width = 1, .height = 1 };

  xcb_render_fill_rectangles(globalconf.connection,
			     XCB_RENDER_PICT_OP_SRC,
			     *alpha_picture,
			     color, 1, &rect);

  xcb_free_pixmap(globalconf.connection, pixmap);
}

/** Paint the root background to the buffer Picture */
static void
render_paint_background(void)
{
  _render_paint_root_background_to_buffer();
}

/** Paint the window to the buffer Picture
 *
 * \param window The window to be painted
 */
static void
render_paint_window(window_t *window)
{
  /* Allocate memory specific to the rendering backend */
  if(!window->rendering)
    window->rendering = calloc(1, sizeof(_render_window_t));

  _render_window_t *render_window = (_render_window_t *) window->rendering;

  /* Create the window if it does not already exist */
  if(render_window->picture == XCB_NONE)
    {
      debug("Creating new picture for window %jx", (uintmax_t) window->id);

      render_window->picture = xcb_generate_id(globalconf.connection);
      const uint32_t create_picture_val = XCB_SUBWINDOW_MODE_CLIP_BY_CHILDREN;

      xcb_render_pictvisual_t *window_pictvisual =
	xcb_render_util_find_visual_format(_render_conf.pict_formats,
					   window->attributes->visual);

      xcb_render_create_picture(globalconf.connection,
				render_window->picture, window->pixmap,
				window_pictvisual->format,
				XCB_RENDER_CP_SUBWINDOW_MODE,
				&create_picture_val);
    }

  uint8_t render_composite_op;
  xcb_render_picture_t window_alpha_picture;

  {
    /* Only  the  opacity  plugins  needs  such  hook  ATM,  but  well
       something more generic will be written if needed */
    plugin_t *opacity_plugin = plugin_search_by_name("opacity");
    uint16_t opacity;

    if(opacity_plugin && opacity_plugin->vtable->window_get_opacity &&
       (opacity = (*opacity_plugin->vtable->window_get_opacity)(window)) != UINT16_MAX)
      {
	if(!render_window->alpha_picture)
	  _render_create_window_alpha_picture(&render_window->alpha_picture,
					      opacity);

	render_composite_op = XCB_RENDER_PICT_OP_OVER;
	window_alpha_picture = render_window->alpha_picture;
      }
    else
      {
	render_composite_op = XCB_RENDER_PICT_OP_SRC;
	window_alpha_picture = XCB_NONE;
      }
  }

  xcb_render_composite(globalconf.connection,
		       render_composite_op,
		       render_window->picture, window_alpha_picture, _render_conf.buffer_picture,
		       0, 0, 0, 0,
		       window->geometry->x,
		       window->geometry->y,
		       (uint16_t) (window->geometry->width + window->geometry->border_width * 2),
		       (uint16_t) (window->geometry->height + window->geometry->border_width * 2));
}

/** Routine to  paint everything on  the root Picture, it  just paints
 *  the contents of the buffer Picture to the root Picture
 */
static void
render_paint_all(void)
{
  /* This step  is necessary  (e.g. don't paint  directly on  the root
     window Picture in  the loop) to avoid flickering  which is really
     annoying */
  _render_paint_root_buffer_to_root();
}

/** Check  whether  the given  request  major  opcode  is from  Render
 *  extension. A X  request is identified by a  major opcode (as given
 *  by  extension  information)  and   the  minor  code  starts  at  0
 *  (therefore a request is uniquely identified by its major and minor
 *  opcodes)
 *
 * \param request_major_code The X request major opcode
 * \return True if this is a Render request
 */
static bool
render_is_request(const uint8_t request_major_code)
{
  return (_render_conf.ext->major_opcode == request_major_code);
}

/** Get the request  label from the given minor  opcode
 *
 * \see render_is_request
 * \param request_minor_code The X request minor opcode
 * \return The X request label associated
 */
static const char *
render_error_get_request_label(const uint16_t request_minor_code)
{
  return (request_minor_code < countof(_render_request_label) ?
	  _render_request_label[request_minor_code] : NULL);
}

/** Get the  error label associated with  the given error  code.  On X
 *  Window System, the  error code of an extension  is relative to its
 *  first error as given by the extension information
 *
 * \param error_code The X error code
 * \return The associated error message
 */
static const char *
render_error_get_error_label(const uint8_t error_code)
{
  const int render_error = error_code - _render_conf.ext->first_error;

  if(render_error < XCB_RENDER_PICT_FORMAT ||
     render_error > XCB_RENDER_GLYPH)
    return NULL;

  return _render_error_label[render_error];
}

/** Free the Picture associated with the window Pixmap
 *
 * \param window The window whose Picture is going to be freed
 */
static void
render_free_window_pixmap(window_t *window)
{
  _render_window_t *render_window = (_render_window_t *) window->rendering;

  if(render_window)
    {
      xcb_render_free_picture(globalconf.connection, render_window->picture);
      render_window->picture = XCB_NONE;
    }
}

/** Free the resources allocated by the backend for the given window
 *
 * \param window The window whose rendering information are going to be freed
 */
static void
render_free_window(window_t *window)
{
  free((_render_window_t *) window->rendering);
}

/** Called on dlclose()  and free all the resources  allocated by this
 *  backend
 */
static void  __attribute__((destructor))
render_free(void)
{
  free(_render_conf.pict_formats);
  xcb_render_free_picture(globalconf.connection, _render_conf.background_picture);
  xcb_render_free_picture(globalconf.connection, _render_conf.picture);
  xcb_render_free_picture(globalconf.connection, _render_conf.buffer_picture);
}

/** Structure holding all the functions addresses */
rendering_t rendering_functions = {
  render_init,
  render_init_finalise,
  render_reset_background,
  render_paint_background,
  render_paint_window,
  render_paint_all,
  render_is_request,
  render_error_get_request_label,
  render_error_get_error_label,
  render_free_window_pixmap,
  render_free_window
};
