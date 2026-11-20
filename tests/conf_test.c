#include "../src/conf.h"
#include "../src/config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

typedef struct {
    char     name[8];
    uint16_t port;
    unsigned ms;
    int      n;
    float    gain;
    bool     on;
    int      mode;
} t_cfg_t;

static const char *const MODES[] = { "a", "b", "c", NULL };

#define F(sec, key, type, member) \
    { sec, #key, type, offsetof(t_cfg_t, member), sizeof(((t_cfg_t *)0)->member), NULL }

static const conf_field_t FIELDS[] = {
    F("s", name, CONF_STR,   name),
    F("s", port, CONF_PORT,  port),
    F("s", ms,   CONF_UINT,  ms),
    F("s", n,    CONF_INT,   n),
    F("s", gain, CONF_FLOAT, gain),
    F("s", on,   CONF_BOOL,  on),
    { "s", "mode", CONF_CHOICE, offsetof(t_cfg_t, mode), 0, MODES },
    { 0 }
};

static char path[64];

static int load(const char *text, t_cfg_t *out, const char *const *open)
{
    FILE *fp = fopen(path, "w");
    fputs(text, fp);
    fclose(fp);
    memset(out, 0, sizeof(*out));
    conf_t c;
    if (conf_read(&c, path) != 0)
        return -1;
    int errors = conf_apply(&c, FIELDS, out, open);
    conf_free(&c);
    return errors;
}

int main(void)
{
    t_cfg_t t;
    snprintf(path, sizeof(path), "/tmp/conf_test.%d", (int)getpid());

    CHECK(load("# comment\n\n[s]\nname = abc   # trailing\nport=4811\nms = 20\nn = -3\n"
               "gain = -1.5\non = true\nmode = c\n", &t, NULL) == 0);
    CHECK(strcmp(t.name, "abc") == 0);
    CHECK(t.port == 4811 && t.ms == 20 && t.n == -3 && t.gain == -1.5f && t.on && t.mode == 2);

    fputs("-- expected errors follow --\n", stderr);
    CHECK(load("[s]\nname = \"abc\"\n", &t, NULL) == 1);              /* quotes */
    CHECK(load("[s]\nport = 1\nport = 2\n", &t, NULL) == 1);          /* duplicate */
    CHECK(load("[s]\nbogus = 1\n", &t, NULL) == 1);                   /* unknown key */
    CHECK(load("[x]\nport = 1\n", &t, NULL) == 1);                    /* unknown section */
    CHECK(load("port = 1\n", &t, NULL) == 1);                         /* no section */
    CHECK(load("[s]\nport = 0\n", &t, NULL) == 1);
    CHECK(load("[s]\nport = 65536\n", &t, NULL) == 1);
    CHECK(load("[s]\nms = -1\n", &t, NULL) == 1);
    CHECK(load("[s]\nn = 12x\n", &t, NULL) == 1);
    CHECK(load("[s]\non = yes\n", &t, NULL) == 1);
    CHECK(load("[s]\nmode = d\n", &t, NULL) == 1);
    CHECK(load("[s]\nname = abcdefgh\n", &t, NULL) == 1);             /* too long */
    CHECK(load("[s\n", &t, NULL) == 1);
    CHECK(load("[s]\njunk\n", &t, NULL) == 1);
    fputs("-- end expected errors --\n", stderr);

    static const char *const OPEN[] = { "peers", NULL };
    CHECK(load("[peers]\nanything = goes here\n", &t, OPEN) == 0);

    unlink(path);

    config_t cfg;
    CHECK(config_load(&cfg, "config/rusrp.conf.example") == 0);
    CHECK(strcmp(cfg.audio.alsa_device, "plughw:1,0") == 0);

    if (failures) {
        fprintf(stderr, "conf_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("conf_test: ok\n");
    return 0;
}
