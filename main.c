// This program is a Wayland client which draws arbitrary text to an EGL window.

/* TODO:
 * - add xdg configure serial to display
 * - move over seat capabilities
 * - move over key logic
 * - move over pty logic
 * */

#include <stdlib.h> 
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <pty.h> 

#include <stdint.h>

#include <cglm/cglm.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <wayland-egl.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h> 

#include <freetype2/ft2build.h>
#include FT_SFNT_NAMES_H
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_BBOX_H
#include FT_TYPE1_TABLES_H


#define MAX(a, b) ((a) > (b) ? a : b)
#define SHELL "/bin/bash"

#define TERM_WIDTH 80
#define TERM_HEIGHT 25

struct point {
	GLfloat surface_x;
	GLfloat surface_y;
	GLfloat texture_x;
	GLfloat texture_y;
};

struct glyph {
	float x_offset;
	float y_offset;
	float x_advance;
	float y_advance;
	float bitmap_width;
	float bitmap_height;
	float bitmap_top;
	float bitmap_left;

};

struct opengl_data {
	GLuint vbo;
	GLuint texture;
	GLint attribute_coord;
	GLint uniform_text;
	GLint uniform_color;
};

struct freetype_data {
	FT_Library value;
	FT_Error status;
	FT_Face face;
	FT_GlyphSlot g;
};

struct texture_data {
	unsigned int max_char_width;
	unsigned int max_char_height;
	float texture_width;
	float texture_height;
};

struct render_data {
	struct display *display;
	struct texture_data *texture_data;
	struct glyph (*glyphs)[128];
	struct opengl_data *gl_data;
	char (*terminal_cells)[TERM_HEIGHT][TERM_WIDTH];
	int term_x;
	int term_y;
};


struct display {
	struct wl_display *wl_display;
	struct wl_compositor *compositor;
	struct xdg_wm_base *xdg_wm_base;
	struct wl_surface *wl_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct wl_egl_window *egl_window;
	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLSurface egl_surface;
	struct xkb_context *context;
	struct wl_list seats;
	struct pty *pty;
};

struct seat {
	struct display *display;
	struct wl_seat *wl_seat;
	struct wl_keyboard *wl_kbd;
    uint32_t version; /* ... of wl_seat */
    uint32_t global_name; /* an ID of sorts */
    char *name_str; /* a descriptor */
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    struct wl_list link;
};

struct pty {
	int master_fd, slave_fd;
};

struct egl {
	EGLint *major, *minor;
	EGLint *n;
	EGLConfig egl_config;
	EGLint *config_attribs;
	EGLint *context_attribs;
};

const GLfloat black[4] = {0, 0, 0, 1};

static const GLchar vertext_shader_src[] =
	"#version 100\n"
	"\n"
	"attribute vec4 coord;\n"
	"varying vec2 textpos;\n"
	"\n"
	"void main(void) {\n"
	"  gl_Position = vec4(coord.xy, 0, 1);\n"
	"  textpos = coord.zw;\n"
	"}\n";

static const GLchar fragtext_shader_src[] =
    "#version 100\n"
    "precision mediump float;\n"
    "\n"
    "varying vec2 textpos;\n"
    "uniform sampler2D text;\n"
    "uniform vec4 color;\n"
    "\n"
    "void main(void) {\n"
    "  gl_FragColor = vec4(1, 1, 1, texture2D(text, textpos).a) * color;\n"
    "}\n";

	
static int width = 800;
static int height = 800;
static uint32_t xdg_configure_serial = 0;
static bool running = true;
static GLuint gl_text_prog = 0;
static void render_cells(struct render_data *callback);

/* PTY CODE */


bool setup_new_tty(struct pty *pty, struct display *display) {
	pid_t p;
	/* indicate that our terminal has no positioning capability other than spaces */
	/* and carriage return, and that it is incapable of procesing escape sequences */
    char *env[] = { "TERM=dumb", NULL };
	if (openpty(&pty->master_fd,&pty->slave_fd,NULL,NULL,NULL) < 0)
		fprintf(stderr,"openpty");
	
	switch (p = fork()) {
	case -1:
		fprintf(stderr,"fork");
		break;
	case 0:
		close(pty->master_fd);
        setsid();
		
        if (ioctl(pty->slave_fd, TIOCSCTTY, NULL) < 0)
        {
			fprintf(stderr,"ioctl(TIOCSCTTY)");
            return false;
        }

        dup2(pty->slave_fd, 0);
        dup2(pty->slave_fd, 1);
        dup2(pty->slave_fd, 2);
        close(pty->slave_fd);
        execle(SHELL, "-" SHELL, (char *)NULL, env);
		break;
	default:
		close(pty->slave_fd);
		display->pty = pty;
		break;
	}
	return true;
}

/* END PTY CODE */

/* BEGIN XKBCOMMON CODE */

void
tools_print_keycode_state(struct xkb_state *state,
                          struct xkb_compose_state *compose_state,
                          xkb_keycode_t keycode,
                          enum xkb_consumed_mode consumed_mode)
{
    struct xkb_keymap *keymap;

    xkb_keysym_t sym;
    const xkb_keysym_t *syms;
    int nsyms;
    char s[16];
    xkb_layout_index_t layout;
    enum xkb_compose_status status;

    keymap = xkb_state_get_keymap(state);

    nsyms = xkb_state_key_get_syms(state, keycode, &syms);

    if (nsyms <= 0)
        return;

    status = XKB_COMPOSE_NOTHING;
    if (compose_state)
        status = xkb_compose_state_get_status(compose_state);

    if (status == XKB_COMPOSE_COMPOSING || status == XKB_COMPOSE_CANCELLED)
        return;

    if (status == XKB_COMPOSE_COMPOSED) {
        sym = xkb_compose_state_get_one_sym(compose_state);
        syms = &sym;
        nsyms = 1;
    }
    else if (nsyms == 1) {
        sym = xkb_state_key_get_one_sym(state, keycode);
        syms = &sym;
    }

    printf("keysyms [ ");
    for (int i = 0; i < nsyms; i++) {
        xkb_keysym_get_name(syms[i], s, sizeof(s));
        printf("%-*s ", (int) sizeof(s), s);
    }
    printf("] ");

    if (status == XKB_COMPOSE_COMPOSED)
        xkb_compose_state_get_utf8(compose_state, s, sizeof(s));
    else
        xkb_state_key_get_utf8(state, keycode, s, sizeof(s));
    printf("unicode [ %s ] ", s);

    layout = xkb_state_key_get_layout(state, keycode);
    printf("layout [ %s (%d) ] ",
           xkb_keymap_layout_get_name(keymap, layout), layout);

    printf("level [ %d ] ",
           xkb_state_key_get_level(state, keycode, layout));

    printf("mods [ ");
    for (xkb_mod_index_t mod = 0; mod < xkb_keymap_num_mods(keymap); mod++) {
        if (xkb_state_mod_index_is_active(state, mod,
                                          XKB_STATE_MODS_EFFECTIVE) <= 0)
            continue;
        if (xkb_state_mod_index_is_consumed2(state, keycode, mod,
                                             consumed_mode))
            printf("-%s ", xkb_keymap_mod_get_name(keymap, mod));
        else
            printf("%s ", xkb_keymap_mod_get_name(keymap, mod));
    }
    printf("] ");

    printf("leds [ ");
    for (xkb_led_index_t led = 0; led < xkb_keymap_num_leds(keymap); led++) {
        if (xkb_state_led_index_is_active(state, led) <= 0)
            continue;
        printf("%s ", xkb_keymap_led_get_name(keymap, led));
    }
    printf("] ");

    printf("\n");
}

static void
kbd_keymap(void *data, struct wl_keyboard *wl_kbd, uint32_t format,
           int fd, uint32_t size)
{
    struct seat *seat = data;
    void *buf;

    buf = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap keymap: %d\n", errno);
        close(fd);
        return;
    }

    seat->keymap = xkb_keymap_new_from_buffer(seat->display->context, buf, size - 1,
                                              XKB_KEYMAP_FORMAT_TEXT_V1,
                                              XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(buf, size);
    close(fd);
    if (!seat->keymap) {
        fprintf(stderr, "Failed to compile keymap!\n");
        return;
    }

    seat->state = xkb_state_new(seat->keymap);
    if (!seat->state) {
        fprintf(stderr, "Failed to create XKB state!\n");
        return;
    }
}

static void
kbd_key(void *data, struct wl_keyboard *wl_kbd, uint32_t serial, uint32_t time,
	uint32_t key, uint32_t state)
{
    struct seat *seat = data;
	struct pty *pty = seat->display->pty;
	int pressed = xkb_state_key_get_one_sym(seat->state,key+8);
	uint32_t keycode = key + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(seat->state,
		keycode, &syms);

	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = syms[i];
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			switch (sym) {
				char c;
			case XKB_KEY_Return:
				c = '\n';
				write(pty->master_fd,&c,1);
				break;
			case 32 ... 126:
				c = (char)sym;
			    write(pty->master_fd,&c,1);
				break;
			default:
				printf("invalid key pressed: %d\n",sym);
				break;
			}
		}
	}

    if (pressed == XKB_KEY_Escape)
        running=false;

		tools_print_keycode_state(seat->state, NULL, key + 8,
		                          XKB_CONSUMED_MODE_XKB);
}


static void
kbd_enter(void *data, struct wl_keyboard *wl_kbd, uint32_t serial,
          struct wl_surface *surf, struct wl_array *keys) {}

static void
kbd_leave(void *data, struct wl_keyboard *wl_kbd, uint32_t serial,
          struct wl_surface *surf) {}


static void
kbd_modifiers(void *data, struct wl_keyboard *wl_kbd, uint32_t serial,
              uint32_t mods_depressed, uint32_t mods_latched,
              uint32_t mods_locked, uint32_t group) {
    struct seat *seat = data;
    xkb_state_update_mask(seat->state, mods_depressed, mods_latched,
                          mods_locked, 0, 0, group);
              }

static void
kbd_repeat_info(void *data, struct wl_keyboard *wl_kbd, int32_t rate,
                int32_t delay) {}

static const struct wl_keyboard_listener kbd_listener = {
    kbd_keymap,
    kbd_enter,
    kbd_leave,
    kbd_key,
    kbd_modifiers,
    kbd_repeat_info
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t caps) {
    struct seat *seat = data;

    if (!seat->wl_kbd && (caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        seat->wl_kbd = wl_seat_get_keyboard(seat->wl_seat);
        wl_keyboard_add_listener(seat->wl_kbd, &kbd_listener, seat);
    }
    else if (seat->wl_kbd && !(caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        if (seat->version >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_keyboard_release(seat->wl_kbd);
        else
            wl_keyboard_destroy(seat->wl_kbd);

        xkb_state_unref(seat->state);
        xkb_keymap_unref(seat->keymap);

        seat->state = NULL;
        seat->keymap = NULL;
        seat->wl_kbd = NULL;
    }
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    struct seat *seat = data;
    free(seat->name_str);
    seat->name_str = strdup(name);
}

static void
seat_destroy(struct seat *seat)
{
    if (seat->wl_kbd) {
        if (seat->version >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_keyboard_release(seat->wl_kbd);
        else
            wl_keyboard_destroy(seat->wl_kbd);
        xkb_state_unref(seat->state);
        xkb_keymap_unref(seat->keymap);
    }
    if (seat->version >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(seat->wl_seat);
    else
        wl_seat_destroy(seat->wl_seat);
    free(seat->name_str);
    wl_list_remove(&seat->link);
    free(seat);
}


static const struct wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name
};

static void seat_create(struct display *display, struct wl_registry *registry,
            uint32_t name, uint32_t version) {
    struct seat *seat = calloc(1, sizeof(*seat));
    seat->global_name = name;
    seat->display = display;
	seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                     MAX(version, 5));
    wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);
    wl_list_insert(&display->seats, &seat->link);
}

/* END XKBCOMMON MODE */

static void frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	wl_callback_destroy(callback);
	struct render_data *cb_data = data;
	render_cells(cb_data);
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_handle_done,
};

static void xdg_surface_handle_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial) {
	xdg_configure_serial = serial;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data,
		struct xdg_toplevel *toplevel, int32_t w, int32_t h,
		struct wl_array *state) {
	if (w > 0) {
		width = w;
	}
	if (h > 0) {
		height = h;
	}
}

static void xdg_toplevel_handle_close(void *data,
		struct xdg_toplevel *xdg_toplevel) {
	running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct display *display = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		display->compositor =
			wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		display->xdg_wm_base =
			wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, "wl_seat") == 0) {
        seat_create(display, registry, name, version);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
	uint32_t name) {
	struct display *display = data;
    struct seat *seat, *tmp;

    wl_list_for_each_safe(seat, tmp, &display->seats, link) {
        if (seat->global_name != name)
            continue;

        seat_destroy(seat);
    }
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static GLuint compile_shader(GLuint type, const GLchar *src) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "Failed to compile shader: %s\n", log);
		return 0;
	}

	return shader;
}

static GLuint compile_text_program(void) {
	GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertext_shader_src);
	if (vertex_shader == 0) {
		fprintf(stderr, "Failed to compile vertex shader\n");
		return 0;
	}

	GLuint fragment_shader =
		compile_shader(GL_FRAGMENT_SHADER, fragtext_shader_src);
	if (fragment_shader == 0) {
		fprintf(stderr, "Failed to compile fragment shader\n");
		return 0;
	}

	GLuint shader_program = glCreateProgram();
	glAttachShader(shader_program, vertex_shader);
	glAttachShader(shader_program, fragment_shader);
	glLinkProgram(shader_program);
	GLint ok;
	glGetProgramiv(shader_program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(shader_program, sizeof(log), NULL, log);
		fprintf(stderr, "Failed to link program: %s\n", log);
		return 0;
	}

	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);

	return shader_program;
}

/* when we draw here, we should be using monospaced vertex coordinates */
static void render_cells(struct render_data *callback) {
	struct opengl_data *gl_data = callback->gl_data;
	struct glyph *glyphs = *(callback->glyphs);
	struct texture_data *texture_data = callback->texture_data;
	struct display *display = callback->display;
	
	if (xdg_configure_serial != 0) {
		wl_egl_window_resize(display->egl_window, width, height, 0, 0);
		xdg_surface_ack_configure(display->xdg_surface, xdg_configure_serial);
		xdg_configure_serial = 0;
	}

	if (!eglMakeCurrent(display->egl_display, display->egl_surface, display->egl_surface, display->egl_context)) {
		fprintf(stderr, "eglMakeCurrent failed\n");
		return;
	}
	glUseProgram(gl_text_prog);
	glViewport(0, 0, width, height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(1, 1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glUniform4fv(gl_data->uniform_color, 1, black);
	glEnableVertexAttribArray(gl_data->attribute_coord);
	glBindBuffer(GL_ARRAY_BUFFER,gl_data->vbo);
	glVertexAttribPointer(gl_data->attribute_coord, 4, GL_FLOAT, GL_FALSE, 0, 0);
	EGLint window_height, window_width;
	eglQuerySurface(display->egl_display,display->egl_surface,EGL_HEIGHT,&window_height);
	eglQuerySurface(display->egl_display,display->egl_surface,EGL_WIDTH,&window_width);
	float sx = 2.0/window_width;
	float sy = 2.0/window_height;

	/* from this point on, GL_TEXTURE_2D becomes an alias for texture */
	glBindTexture(GL_TEXTURE_2D,gl_data->texture);
	glUniform1i(gl_data->uniform_text, 0);

	/* draw the grid */

	struct point coords[6 * 80 * 25];
	int c = 0;
	int i;
	int j;
	for(i = 0; i < 25; i++) {
		for(j = 0; j < 80; j++) {
			int current_cell = (int)(*(callback->terminal_cells))[i][j];
			if(current_cell == 0) {
				continue;
			}
			float x = -1 + j*texture_data->max_char_width*sx;
			float y = 1 - i*texture_data->max_char_height*sy - 50*sy;
			float x2 = x + glyphs[current_cell].bitmap_left * sx;
			float y2 =-y -glyphs[current_cell].bitmap_top * sy;
			float w2 = glyphs[current_cell].bitmap_width * sx;
			float h2 = glyphs[current_cell].bitmap_height * sy;
			float glyph_width = glyphs[current_cell].bitmap_width / texture_data->texture_width;
			float glyph_height = glyphs[current_cell].bitmap_height / texture_data->texture_height;
			coords[c++] = (struct point) {
				x2, -y2, glyphs[current_cell].x_offset, glyphs[current_cell].y_offset		};
			coords[c++] = (struct point) {
				x2 + w2, -y2, glyphs[current_cell].x_offset + glyph_width, glyphs[current_cell].y_offset
			};
			coords[c++] = (struct point) {
				x2, -y2 - h2, glyphs[current_cell].x_offset, glyphs[current_cell].y_offset + glyph_height
			};
			coords[c++] = (struct point) {
				x2 + w2, -y2, glyphs[current_cell].x_offset + glyph_width, glyphs[current_cell].y_offset
			};
			coords[c++] = (struct point) {
				x2, -y2 - h2, glyphs[current_cell].x_offset, glyphs[current_cell].y_offset + glyph_height
			};
			coords[c++] = (struct point) {
				x2 + w2, -y2 - h2, glyphs[current_cell].x_offset + glyph_width, glyphs[current_cell].y_offset + glyph_height
			};
		}
	}
	glBufferData(GL_ARRAY_BUFFER,sizeof coords, coords, GL_DYNAMIC_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, c);
	glDisableVertexAttribArray(gl_data->attribute_coord);
	glUseProgram(0);
	struct wl_callback *wl_callback = wl_surface_frame(display->wl_surface);
	/* create a struct w/ texture map params and add it here */
	wl_callback_add_listener(wl_callback, &frame_listener, callback);
	if (!eglSwapBuffers(display->egl_display, display->egl_surface)) {
		fprintf(stderr, "eglSwapBuffers failed\n");
	}
}

/* we should no longer create a monospaced texture */
void create_texture(struct freetype_data *ft_data, struct opengl_data *gl_data, struct texture_data *texture_data, struct glyph *glyphs) {
	ft_data->g = ft_data->face->glyph;
	glActiveTexture(GL_TEXTURE0);
	/* store one texture name in texture param */
	glGenTextures(1,&gl_data->texture);
	/* create or use named texture */
	/* from this point on, GL_TEXTURE_2D becomes an alias for texture */
	glBindTexture(GL_TEXTURE_2D,gl_data->texture);
	glUniform1i(gl_data->uniform_text, 0);
	/* when pixels are read from client memory, require byte alignment */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	int i;
	int offset_x = 0;
	printf("max advance from size: %hd\n",ft_data->g->face->max_advance_width);
	for(i=32; i < 128; i++) {
		if(FT_Load_Char(ft_data->face,i,FT_LOAD_RENDER)) {
			fprintf(stderr, "Loading character %c failed.\n",i);
			continue;
		}
		texture_data->texture_width += ft_data->g->bitmap.width + 1;
		texture_data->max_char_width = MAX(ft_data->g->bitmap.width,texture_data->max_char_width);
		texture_data->max_char_height = MAX(ft_data->g->bitmap.rows,texture_data->max_char_height);
	}
	texture_data->texture_height = texture_data->max_char_height;
	/*
	 * Allow elements of image array to be read by shaders
	 * Data is read from bitmap.buffer as sequence of unsigned bytes
	 * and grouped into sets of one value to form elements
	 * */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, texture_data->texture_width, texture_data->texture_height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, ft_data->g->bitmap.buffer);
	for(i=32; i < 128; i++) {
		if(FT_Load_Char(ft_data->face,i,FT_LOAD_RENDER)) {
			fprintf(stderr, "Loading character %c failed.\n",i);
			continue;
		}
		 /* redefine contiguous subregion of array texture image */
		glTexSubImage2D(GL_TEXTURE_2D, 0, offset_x, 0, ft_data->g->bitmap.width, ft_data->g->bitmap.rows, GL_ALPHA, GL_UNSIGNED_BYTE, ft_data->g->bitmap.buffer);
		glyphs[i] = (struct glyph) {
			offset_x / (float)texture_data->texture_width,
			0,
			ft_data->g->advance.x >> 6,
			ft_data->g->advance.y >> 6,
			ft_data->g->bitmap.width,
			ft_data->g->bitmap.rows,
			ft_data->g->bitmap_top,
			ft_data->g->bitmap_left
		};
		offset_x += ft_data->g->bitmap.width + 1;
	}
}

void init_terminal_cells(char terminal_cells[25][80]) {
	int i,j;
	for(i=0;i<25;i++) {
		for(j=0;j<80;j++) {
			terminal_cells[i][j]=0;
		}
	}
}

void init_gl_stuff(struct freetype_data *ft_data, struct opengl_data *gl_data, struct texture_data *texture_data) {
	const char * filename =
		"/usr/share/fonts/TTF/Inconsolata-Regular.ttf";
	ft_data->status = FT_Init_FreeType (& ft_data->value);
    if (ft_data->status != 0) {
		fprintf (stderr, "Error %d opening library.\n", ft_data->status);
		exit (EXIT_FAILURE);
    }
    ft_data->status = FT_New_Face (ft_data->value, filename, 0, & ft_data->face);
    if (ft_data->status != 0) {
		fprintf (stderr, "Error %d opening %s.\n", ft_data->status, filename);
		exit (EXIT_FAILURE);
    }
	gl_text_prog = compile_text_program();
	if(gl_text_prog == 0) {
		fprintf(stderr, "failed to compile shader program\n");
	}
	gl_data->attribute_coord = glGetAttribLocation(gl_text_prog, "coord");
	gl_data->uniform_text = glGetUniformLocation(gl_text_prog, "text");
	gl_data->uniform_color = glGetUniformLocation(gl_text_prog, "color");
	if(gl_data->attribute_coord == -1 || gl_data->uniform_text == -1 || gl_data->uniform_color == -1)
		fprintf(stderr,"failed to get shader attr or uniform\n");
	glGenBuffers(1,&gl_data->vbo);
	FT_Set_Pixel_Sizes(ft_data->face, 0, 24);
    texture_data->texture_width = 0;
	texture_data->texture_height = 0;
	texture_data->max_char_height = 0;
	texture_data->max_char_width = 0;
}

// Wayland Client Methods

int display_connect(struct display *display) {
	display->wl_display = wl_display_connect(NULL);
	display->compositor = NULL;
	display->xdg_wm_base = NULL;
	display->wl_surface = NULL;
	display->xdg_surface = NULL;
	display->xdg_toplevel = NULL;
	display->egl_window = NULL;
	display->egl_display = EGL_NO_DISPLAY;
	display->egl_context = EGL_NO_CONTEXT;
	display->egl_surface = EGL_NO_SURFACE;
	display->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (display->wl_display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return 1;
	}
   	display->egl_display = eglGetDisplay((EGLNativeDisplayType)display->wl_display);
	if(!display->context) {
        fprintf(stderr, "Couldn't create xkb context\n");
		return 1;
    }
	return 0;
}

void display_disconnect(struct display *display) {
	if(display->xdg_toplevel)
		xdg_toplevel_destroy(display->xdg_toplevel);
	if(display->xdg_surface)
		xdg_surface_destroy(display->xdg_surface);
	if(display->wl_surface)
		wl_surface_destroy(display->wl_surface);
	if(display->compositor)
		wl_compositor_destroy(display->compositor);
	xkb_context_unref(display->context);
	wl_display_disconnect(display->wl_display);
}

/* TODO: rename this */
int wl_initialize_compositor(struct wl_registry *registry, struct display *display) {
	wl_registry_add_listener(registry, &registry_listener, display);
	wl_display_dispatch(display->wl_display);
	wl_display_roundtrip(display->wl_display);
	if (display->compositor == NULL || display->xdg_wm_base == NULL) {
		fprintf(stderr, "no wl_shm, wl_compositor or xdg_wm_base support\n");
		return 1;
	}
	return 0;
}

/* TODO: rename */
int wl_initialize_egl(struct display *display, struct egl *egl) {
	 if (!eglInitialize(display->egl_display, egl->major, egl->minor)) {
		fprintf(stderr, "failed to initialize EGL\n");
		return 1;
	}
	 	display->egl_display = eglGetDisplay((EGLNativeDisplayType)display->wl_display);
	if (display->egl_display == EGL_NO_DISPLAY) {
		fprintf(stderr, "failed to create EGL display\n");
		return 1;
	}
		eglChooseConfig(display->egl_display, egl->config_attribs, &egl->egl_config, 1, egl->n);
	if (egl->n == 0) {
		fprintf(stderr, "failed to choose an EGL config\n");
		return 1;
	}
	
	display->egl_context = eglCreateContext(display->egl_display, egl->egl_config,
		EGL_NO_CONTEXT, egl->context_attribs);
	if (display->egl_context == EGL_NO_CONTEXT) {
		fprintf(stderr, "eglCreateContext failed\n");
		return 1;
	}
	return 0;
}

/* TODO: rename */
int wl_init_surface(struct display *display) {
	display->wl_surface = wl_compositor_create_surface(display->compositor);
	display->xdg_surface = xdg_wm_base_get_xdg_surface(display->xdg_wm_base, display->wl_surface);
	display->xdg_toplevel = xdg_surface_get_toplevel(display->xdg_surface);
	xdg_surface_add_listener(display->xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel_add_listener(display->xdg_toplevel, &xdg_toplevel_listener, NULL);
	return 0;
}

int more_egl_init(struct display *display, struct egl *egl) {
	display->egl_window = wl_egl_window_create(display->wl_surface, width, height);
	if (display->egl_window == NULL) {
		fprintf(stderr, "wl_egl_window_create failed\n");
		return 1;
	}
	display->egl_surface = eglCreateWindowSurface(display->egl_display, egl->egl_config,
		(EGLNativeWindowType)display->egl_window, NULL);
	if (display->egl_surface == EGL_NO_SURFACE) {
		fprintf(stderr, "eglCreateWindowSurface failed\n");
		return 1;
	}
	if (!eglMakeCurrent(display->egl_display, display->egl_surface, display->egl_surface, display->egl_context)) {
		fprintf(stderr, "eglMakeCurrent failed\n");
		return 1;
	}
	eglSwapInterval(display->egl_display, 0);
	return 0;
}
static void shift_cells_up_displacing_top(struct render_data *render_data) {
	render_data->term_x=0;
	int i,j;
	for(i=0;i<24;i++) {
		for(j=0;j<80;j++) {
			(*(render_data->terminal_cells))[i][j] = (*(render_data->terminal_cells))[i+1][j];
		}
	}
	for(j=0;j<80;j++) {
		(*(render_data->terminal_cells))[24][j] = 0; 
	}

}
void add_new_line(struct render_data *render_data) {
	if(render_data->term_y < TERM_HEIGHT-1) {
		render_data->term_x = 0;
		render_data->term_y++;
	} 
	else {
		shift_cells_up_displacing_top(render_data);
	}
}
int read_shell_input(int fd, struct render_data *render_data) {
	char buf[1];
	if(read(fd,buf,1) <= 0) {
		fprintf(stderr,"failed to read codepoint");
		return -1;
	}
	fprintf(stdout,"read: %d",buf[0]);
	if(buf[0] == '\r' || buf[0] == '\n') {
		add_new_line(render_data);
		return 0;
	}
	if(buf[0] < 32) {
		fprintf(stdout,"invalid codepoint: %d",buf[0]);
		return -1;
	}
	(*(render_data->terminal_cells))[render_data->term_y][render_data->term_x] = buf[0];
	if(buf[0] >= 32) {
		if(render_data->term_x == TERM_WIDTH && render_data->term_y < TERM_HEIGHT) {
			add_new_line(render_data);
		}
		else if (render_data->term_x < 80) {
			render_data->term_x++;
		}
		return 0;
	}
	fprintf(stdout,"invalid codepoint: %d",buf[0]);
	return -1;
}

int main(int argc, char *argv[]) {
	struct display display;
	display_connect(&display);
	wl_list_init(&display.seats);
	struct wl_registry *registry = wl_display_get_registry(display.wl_display);
	if(wl_initialize_compositor(registry,&display) > 0) {
		fprintf(stderr,"error initializing compositor\n");
	}
	EGLint major = 0, minor = 0, n = 0;
	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE,
	};
	EGLConfig egl_config;
	EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE,
	};
	struct egl egl  = {&major,&minor,&n,&egl_config,config_attribs,context_attribs};
	wl_initialize_egl(&display,&egl);
	wl_init_surface(&display);
	wl_surface_commit(display.wl_surface);
	wl_display_roundtrip(display.wl_display);
	if(more_egl_init(&display,&egl) > 0) {
		return 1;
	}
	struct glyph glyphs[128];
	struct freetype_data ft_data;
	struct opengl_data gl_data;
	struct texture_data texture_data;
	init_gl_stuff(&ft_data, &gl_data, &texture_data);
	create_texture(&ft_data, &gl_data, &texture_data, glyphs);
	/* declare grid of pointers to glyph
	 * pass it to render_cells
	 * render cells iterates over it and draws as long as there's glyphs
	 * also needs to be in callback */
	char terminal_cells[25][80];
	init_terminal_cells(terminal_cells);
	/* struct render_data callback = {&texture_data,glyphs,&gl_data,terminal_cells}; */
	struct render_data callback;
	callback.texture_data = &texture_data;
	callback.glyphs = &glyphs;
	callback.gl_data = &gl_data;
	callback.terminal_cells = &terminal_cells;
	callback.display = &display;
	callback.term_x = 0;
	callback.term_y = 0;
	struct pty pty = {0,0};
	setup_new_tty(&pty,&display);
	struct pollfd fds[2];
	/* get wayland fd */
	fds[0].fd = wl_display_get_fd(display.wl_display);
	fds[0].events = POLLIN|POLLPRI;
	fds[0].revents = 0;
	fds[1].fd = pty.master_fd;
	fds[1].events = POLLIN|POLLPRI;
	fds[1].revents = 0;
	render_cells(&callback);
	while(running) {
		int r = poll(fds, 2, 500);
		if(fds[0].revents & POLLIN) {
			if(wl_display_dispatch(display.wl_display) == -1)
				running = false;
		}
		if(fds[0].revents & POLLHUP) {
			fprintf(stderr,"pollhup in wldisplay fd");
			wl_display_dispatch(display.wl_display);
		}
		if(fds[1].revents & POLLIN) {
			read_shell_input(fds[1].fd, &callback);
		}
		if(fds[1].revents & POLLHUP) {
			fprintf(stderr,"pollhup in wldisplay fd");
		}
		if(r < 0) {
			fprintf(stderr,"OH MY GOD!!!!!!!");
			return 1;
		}
	}
	display_disconnect(&display);
	return 0;
}
