WAYLAND_FLAGS = `pkg-config wayland-client wayland-egl --cflags --libs`
GL_FLAGS = `pkg-config egl glesv2 --cflags --libs`
CGLM_FLAGS = `pkg-config cglm --cflags --libs`
FT_FLAGS = `pkg-config freetype2 --cflags --libs`
XKB_FLAGS = `pkg-config xkbcommon --cflags --libs`
WAYLAND_PROTOCOLS_DIR = `pkg-config wayland-protocols --variable=pkgdatadir`
WAYLAND_SCANNER = `pkg-config --variable=wayland_scanner wayland-scanner`
CFLAGS ?= -Wall -g

XDG_SHELL_PROTOCOL = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

XDG_SHELL_FILES=xdg-shell-client-protocol.h xdg-shell-protocol.c

all: gl_text

gl_text: main.c $(XDG_SHELL_FILES)
	$(CC) $(CFLAGS) -o gl_text $(WAYLAND_FLAGS) $(GL_FLAGS) $(CGLM_FLAGS) $(FT_FLAGS) $(XKB_FLAGS) -lutil *.c

xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL_PROTOCOL) xdg-shell-client-protocol.h

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) code $(XDG_SHELL_PROTOCOL) xdg-shell-protocol.c

.PHONY: clean
clean:
	$(RM) gl_text $(XDG_SHELL_FILES)
