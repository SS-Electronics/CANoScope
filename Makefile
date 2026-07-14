# CANoScope – Makefile
#
# Build:   make               → build/canoscope  (release)
#          make DEBUG=1       → build/canoscope  (debug)
# Run:     ./build/canoscope
# Clean:   make clean         → removes build/ entirely
#
# One-shot install (dependencies + compile + system install + desktop entry):
#          sudo make install
# Only install build/runtime dependencies:
#          sudo make install-deps
# Remove an installed copy:
#          sudo make uninstall

# ------------------------------------------------------------------ #
# Install layout (override with PREFIX=… DESTDIR=…)                    #
# ------------------------------------------------------------------ #
PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR   := $(PREFIX)/bin
SHAREDIR := $(PREFIX)/share/canoscope
APPDIR   := $(PREFIX)/share/applications
ICONDIR  := $(PREFIX)/share/icons/hicolor/256x256/apps

# ------------------------------------------------------------------ #
# Directories                                                          #
# ------------------------------------------------------------------ #
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj

# ------------------------------------------------------------------ #
# Toolchain & flags                                                    #
# ------------------------------------------------------------------ #
TARGET  := $(BUILD_DIR)/canoscope

CC      ?= gcc

# EXTRA_CFLAGS / EXTRA_LDFLAGS let packaging inject hardening flags
# (e.g. from `dpkg-buildflags`) without disturbing the base flags.
EXTRA_CFLAGS  ?=
EXTRA_LDFLAGS ?=

CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 \
           -D_GNU_SOURCE \
           $(shell pkg-config --cflags gtk+-3.0) \
           -Iinc

LDFLAGS := $(shell pkg-config --libs gtk+-3.0) -lpthread -lm

ifeq ($(DEBUG),1)
CFLAGS  += -g -O0 -DDEBUG
else
CFLAGS  += -O2
endif

CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

# ------------------------------------------------------------------ #
# Sources → objects (mirror source tree under build/obj/)             #
# ------------------------------------------------------------------ #
SRCS := main.c \
        driver/drv_can.c \
        driver/socketcan.c \
        driver/dbc.c \
        analysis/bit_analysis.c \
        gui/threads.c \
        gui/message_view.c \
        gui/main_window.c \
        gui/bit_analysis_view.c \
        gui/signal_view.c \
        gui/signal_plot.c \
        gui/math_view.c \
        gui/db_creation.c \
        gui/settings_dialog.c \
        gui/transmit_dialog.c

OBJS := $(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS))

# Subdirectories that must exist before compiling
OBJ_SUBDIRS := $(OBJ_DIR) \
               $(OBJ_DIR)/driver \
               $(OBJ_DIR)/analysis \
               $(OBJ_DIR)/gui

# ------------------------------------------------------------------ #
# Rules                                                                #
# ------------------------------------------------------------------ #
.PHONY: all clean install install-deps install-files uninstall run docs docs-clean test-bit-analysis

all: $(TARGET)

# Link
$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Build successful → $@"

# Compile each source into build/obj/<path>.o
$(OBJ_DIR)/%.o: %.c | $(OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c $< -o $@

# Create output directories on demand
$(BUILD_DIR) $(OBJ_SUBDIRS):
	mkdir -p $@

# Rebuild when any header changes
$(OBJS): $(wildcard inc/*.h)

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory."

# ------------------------------------------------------------------ #
# Documentation (Doxygen → ./docs/index.html)                         #
# ------------------------------------------------------------------ #
docs:
	@command -v doxygen >/dev/null 2>&1 || { \
		echo "doxygen not found. Install it: sudo apt install doxygen graphviz"; \
		exit 1; }
	doxygen Doxyfile
	@echo "Documentation generated → docs/index.html"

docs-clean:
	rm -rf docs
	@echo "Removed generated documentation."

run: all
	@./$(TARGET)

test-bit-analysis: $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_bit_analysis.c analysis/bit_analysis.c driver/dbc.c -o $(BUILD_DIR)/test_bit_analysis $(LDFLAGS)
	@$(BUILD_DIR)/test_bit_analysis

# Install system build/runtime dependencies (auto-detects apt/dnf/pacman).
install-deps:
	@echo "==> Installing dependencies (requires root)…"
	@bash scripts/install_dependencies.sh

# Full end-user install: dependencies → compile → install binary + desktop entry
# + icon.  Run as root:  sudo make install
install: install-deps all install-files
	-update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	-gtk-update-icon-cache -q -f -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null || true
	@echo ""
	@echo "==> CANoScope installed successfully."
	@echo "    Launch it from your application menu or run: canoscope"

# Copy the built binary and application resources into the system.  This target
# performs no icon/desktop cache updates so it is safe for packaging (the .deb
# maintainer scripts/triggers handle caches at install time).
install-files: $(TARGET)
	@echo "==> Installing files to $(DESTDIR)$(PREFIX)…"
	install -Dm755 $(TARGET)                  $(DESTDIR)$(BINDIR)/canoscope
	install -Dm644 assets/taksys_logo.png     $(DESTDIR)$(SHAREDIR)/taksys_logo.png
	install -Dm644 assets/demo.dbc            $(DESTDIR)$(SHAREDIR)/demo.dbc
	install -Dm644 assets/canoscope.png       $(DESTDIR)$(ICONDIR)/canoscope.png
	install -Dm644 assets/canoscope.desktop   $(DESTDIR)$(APPDIR)/canoscope.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/canoscope
	rm -f $(DESTDIR)$(APPDIR)/canoscope.desktop
	rm -f $(DESTDIR)$(ICONDIR)/canoscope.png
	rm -rf $(DESTDIR)$(SHAREDIR)
	-update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	@echo "Uninstalled canoscope."
