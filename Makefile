# Configurable install paths
PREFIX     ?= /usr/local
SYSCONFDIR ?= /etc
BINDIR      = $(PREFIX)/bin
UNITDIR    ?= $(PREFIX)/lib/systemd/system
UDEVDIR    ?= /lib/udev/rules.d

CC      ?= gcc
CFLAGS  += -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -fPIE \
            $(shell pkg-config --cflags alsa libsystemd)
LDFLAGS += -pie
LDLIBS  += $(shell pkg-config --libs alsa libsystemd) -lm -lpthread

BUILDDIR  = build
VENDORDIR = vendor/tomlc99

SRCS = src/main.c \
       src/config.c \
       src/usrp_protocol.c \
       src/usrp_transport.c \
       src/audio_alsa.c \
       src/audio_processing.c \
       src/audio_trim.c \
       src/logic_hid.c \
       src/jitter_buffer.c \
       src/telemetry.c \
       src/watchdog.c \
       $(VENDORDIR)/toml.c

OBJS     = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
DEPFILES = $(OBJS:.o=.d)
TARGET   = $(BUILDDIR)/rusrp

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Application sources — include vendored headers, generate dependency files
$(BUILDDIR)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(VENDORDIR) -MMD -MP -c -o $@ $<

# Vendor source — suppress upstream warnings we cannot fix
$(BUILDDIR)/$(VENDORDIR)/%.o: $(VENDORDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Wno-unused-result -MMD -MP -c -o $@ $<

-include $(DEPFILES)

clean:
	rm -rf $(BUILDDIR)

install: $(TARGET)
	install -Dm755 $(TARGET)                 $(DESTDIR)$(BINDIR)/rusrp
	install -Dm644 config/rusrp.toml.example $(DESTDIR)$(SYSCONFDIR)/rusrp/rusrp.toml.example
	install -Dm644 systemd/rusrp.service     $(DESTDIR)$(UNITDIR)/rusrp.service
	install -Dm644 udev/90-cm119a.rules      $(DESTDIR)$(UDEVDIR)/90-cm119a.rules
	@if [ -z "$(DESTDIR)" ]; then \
		if [ ! -f $(SYSCONFDIR)/rusrp/rusrp.toml ]; then \
			cp $(SYSCONFDIR)/rusrp/rusrp.toml.example $(SYSCONFDIR)/rusrp/rusrp.toml; \
			echo "rusrp: installed default config at $(SYSCONFDIR)/rusrp/rusrp.toml"; \
			echo "rusrp: edit $(SYSCONFDIR)/rusrp/rusrp.toml (set remote_host at minimum) before starting the service"; \
		else \
			echo "rusrp: existing config preserved at $(SYSCONFDIR)/rusrp/rusrp.toml"; \
		fi; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/rusrp
	rm -f $(DESTDIR)$(UNITDIR)/rusrp.service
	rm -f $(DESTDIR)$(UDEVDIR)/90-cm119a.rules
	@echo "rusrp: config at $(SYSCONFDIR)/rusrp/ preserved — remove manually if desired"
