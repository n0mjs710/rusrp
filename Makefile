# Configurable install paths
PREFIX     ?= /usr/local
SYSCONFDIR ?= /etc
BINDIR      = $(PREFIX)/bin
UNITDIR    ?= $(PREFIX)/lib/systemd/system
UDEVDIR    ?= /lib/udev/rules.d

CC      ?= gcc
CFLAGS  += -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -fPIE \
            $(shell pkg-config --cflags alsa libsystemd opus)
LDFLAGS += -pie
LDLIBS  += $(shell pkg-config --libs alsa libsystemd opus) -lm -lpthread

BUILDDIR  = build

# ── rusrp ────────────────────────────────────────────────────────────────────

SRCS = src/main.c \
       src/config.c \
       src/conf.c \
       src/usrp_protocol.c \
       src/usrp_transport.c \
       src/audio_alsa.c \
       src/audio_processing.c \
       src/audio_trim.c \
       src/logic_hid.c \
       src/jitter_buffer.c \
       src/telemetry.c \
       src/watchdog.c \
       src/opus_codec.c

OBJS     = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
DEPFILES = $(OBJS:.o=.d)
TARGET   = $(BUILDDIR)/rusrp

# ── usrp-reflector ───────────────────────────────────────────────────────────

REFL_SRCS = src/reflector.c src/usrp_protocol.c
REFL_OBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(REFL_SRCS))
REFL_DEPS = $(REFL_OBJS:.o=.d)
REFLECTOR = $(BUILDDIR)/usrp-reflector

# ── opus-gateway ──────────────────────────────────────────────────────────────

GW_SRCS = src/opus_gateway.c \
           src/usrp_protocol.c \
           src/opus_codec.c \
           src/conf.c
GW_OBJS  = $(patsubst %.c,$(BUILDDIR)/%.o,$(GW_SRCS))
GW_DEPS  = $(GW_OBJS:.o=.d)
GW_LIBS  = $(shell pkg-config --libs opus libsystemd) -lm
GATEWAY  = $(BUILDDIR)/opus-gateway

# ── tests ────────────────────────────────────────────────────────────────────

JB_TEST      = $(BUILDDIR)/tests/jitter_buffer_test
JB_TEST_OBJS = $(BUILDDIR)/src/jitter_buffer.o
JB_TEST_DEPS = $(BUILDDIR)/tests/jitter_buffer_test.d $(JB_TEST_OBJS:.o=.d)

AP_TEST      = $(BUILDDIR)/tests/audio_processing_test
AP_TEST_OBJS = $(BUILDDIR)/src/audio_processing.o
AP_TEST_DEPS = $(BUILDDIR)/tests/audio_processing_test.d $(AP_TEST_OBJS:.o=.d)

CF_TEST      = $(BUILDDIR)/tests/conf_test
CF_TEST_OBJS = $(BUILDDIR)/src/conf.o $(BUILDDIR)/src/config.o
CF_TEST_DEPS = $(BUILDDIR)/tests/conf_test.d $(CF_TEST_OBJS:.o=.d)

# ── rules ─────────────────────────────────────────────────────────────────────

.PHONY: all clean install uninstall check

all: $(TARGET) $(REFLECTOR) $(GATEWAY)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(REFLECTOR): $(REFL_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(GATEWAY): $(GW_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(GW_LIBS)

$(BUILDDIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(JB_TEST): $(BUILDDIR)/tests/jitter_buffer_test.o $(JB_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread

$(AP_TEST): $(BUILDDIR)/tests/audio_processing_test.o $(AP_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(CF_TEST): $(BUILDDIR)/tests/conf_test.o $(CF_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(shell pkg-config --libs libsystemd)

# Run from the repo root.
check: $(JB_TEST) $(AP_TEST) $(CF_TEST)
	./$(JB_TEST)
	./$(AP_TEST)
	./$(CF_TEST)

$(BUILDDIR)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPFILES) $(REFL_DEPS) $(GW_DEPS) $(JB_TEST_DEPS) $(AP_TEST_DEPS) $(CF_TEST_DEPS)

clean:
	rm -rf $(BUILDDIR)

install: $(TARGET) $(REFLECTOR) $(GATEWAY)
	install -Dm755 $(TARGET)                        $(DESTDIR)$(BINDIR)/rusrp
	install -Dm755 $(REFLECTOR)                     $(DESTDIR)$(BINDIR)/usrp-reflector
	install -Dm755 $(GATEWAY)                       $(DESTDIR)$(BINDIR)/opus-gateway
	install -Dm644 config/rusrp.conf.example        $(DESTDIR)$(SYSCONFDIR)/rusrp/rusrp.conf.example
	install -Dm644 config/opus-gateway.conf.example $(DESTDIR)$(SYSCONFDIR)/rusrp/opus-gateway.conf.example
	install -Dm644 systemd/rusrp.service     $(DESTDIR)$(UNITDIR)/rusrp.service
	install -Dm644 udev/90-cm119a.rules      $(DESTDIR)$(UDEVDIR)/90-cm119a.rules
	@if [ -z "$(DESTDIR)" ]; then \
		if [ ! -f $(SYSCONFDIR)/rusrp/rusrp.conf ]; then \
			cp $(SYSCONFDIR)/rusrp/rusrp.conf.example $(SYSCONFDIR)/rusrp/rusrp.conf; \
			echo "rusrp: installed default config at $(SYSCONFDIR)/rusrp/rusrp.conf"; \
			echo "rusrp: edit $(SYSCONFDIR)/rusrp/rusrp.conf (set remote_host at minimum) before starting the service"; \
		else \
			echo "rusrp: existing config preserved at $(SYSCONFDIR)/rusrp/rusrp.conf"; \
		fi; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/rusrp
	rm -f $(DESTDIR)$(BINDIR)/usrp-reflector
	rm -f $(DESTDIR)$(BINDIR)/opus-gateway
	rm -f $(DESTDIR)$(UNITDIR)/rusrp.service
	rm -f $(DESTDIR)$(UDEVDIR)/90-cm119a.rules
	@echo "rusrp: config at $(SYSCONFDIR)/rusrp/ preserved — remove manually if desired"
