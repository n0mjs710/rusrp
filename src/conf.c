#include "conf.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void conf_err(conf_t *c, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (line > 0)
        fprintf(stderr, "%s:%d: ", c->path, line);
    else
        fprintf(stderr, "%s: ", c->path);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    c->errors++;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static bool valid_name(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-')
            return false;
    return true;
}

int conf_read(conf_t *c, const char *path)
{
    memset(c, 0, sizeof(*c));
    c->path = path;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char   *buf = NULL;
    size_t  bufsz = 0;
    int     lineno = 0;
    char    section[sizeof(c->v[0].section)] = "";

    while (getline(&buf, &bufsz, fp) != -1) {
        lineno++;
        char *hash = strchr(buf, '#');
        if (hash)
            *hash = '\0';
        char *line = trim(buf);
        if (!line[0])
            continue;

        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (!close || close[1] != '\0') {
                conf_err(c, lineno, "malformed section header: %s", line);
                continue;
            }
            *close = '\0';
            char *name = trim(line + 1);
            if (!valid_name(name) || strlen(name) >= sizeof(section)) {
                conf_err(c, lineno, "invalid section name: [%s]", name);
                continue;
            }
            strcpy(section, name);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            conf_err(c, lineno, "expected key = value: %s", line);
            continue;
        }
        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        if (!valid_name(key)) {
            conf_err(c, lineno, "'%s' is not a valid setting name", key);
            continue;
        }
        if (strchr(value, '"') || strchr(value, '\'')) {
            conf_err(c, lineno, "%s: values don't take quotes", key);
            continue;
        }
        bool dup = false;
        for (int i = 0; i < c->n; i++) {
            if (strcmp(c->v[i].section, section) == 0 && strcmp(c->v[i].key, key) == 0) {
                conf_err(c, lineno, "%s: already set on line %d", key, c->v[i].line);
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        if (c->n >= c->cap) {
            int cap = c->cap ? c->cap * 2 : 64;
            conf_entry_t *v = realloc(c->v, (size_t)cap * sizeof(*v));
            if (!v) {
                conf_err(c, lineno, "out of memory");
                break;
            }
            c->v = v;
            c->cap = cap;
        }
        conf_entry_t *e = &c->v[c->n++];
        e->line = lineno;
        strcpy(e->section, section);
        e->key = strdup(key);
        e->value = strdup(value);
    }

    free(buf);
    fclose(fp);
    return 0;
}

void conf_free(conf_t *c)
{
    for (int i = 0; i < c->n; i++) {
        free(c->v[i].key);
        free(c->v[i].value);
    }
    free(c->v);
    c->v = NULL;
    c->n = c->cap = 0;
}

bool conf_parse_ll(const char *s, long long *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s || *end)
        return false;
    *out = v;
    return true;
}

static void store(conf_t *c, const conf_entry_t *e, const conf_field_t *f, void *dst)
{
    char *p = (char *)dst + f->off;
    const char *v = e->value;
    long long l;

    switch (f->type) {
    case CONF_STR:
        if (strlen(v) >= f->size)
            conf_err(c, e->line, "%s: longer than %zu characters", e->key, f->size - 1);
        else
            strcpy(p, v);
        return;
    case CONF_PORT:
        if (!conf_parse_ll(v, &l) || l < 1 || l > 65535)
            conf_err(c, e->line, "%s: '%s' is not a port (1-65535)", e->key, v);
        else
            *(uint16_t *)p = (uint16_t)l;
        return;
    case CONF_UINT:
        if (!conf_parse_ll(v, &l) || l < 0 || l > (long long)UINT32_MAX)
            conf_err(c, e->line, "%s: '%s' is not a whole number >= 0", e->key, v);
        else
            *(unsigned int *)p = (unsigned int)l;
        return;
    case CONF_INT:
        if (!conf_parse_ll(v, &l) || l < INT32_MIN || l > INT32_MAX)
            conf_err(c, e->line, "%s: '%s' is not a whole number", e->key, v);
        else
            *(int *)p = (int)l;
        return;
    case CONF_FLOAT: {
        char *end;
        errno = 0;
        double d = strtod(v, &end);
        if (errno || end == v || *end)
            conf_err(c, e->line, "%s: '%s' is not a number", e->key, v);
        else
            *(float *)p = (float)d;
        return;
    }
    case CONF_BOOL:
        if (strcmp(v, "true") == 0)
            *(bool *)p = true;
        else if (strcmp(v, "false") == 0)
            *(bool *)p = false;
        else
            conf_err(c, e->line, "%s: '%s' is not true or false", e->key, v);
        return;
    case CONF_CHOICE:
        for (int i = 0; f->choices[i]; i++) {
            if (strcmp(v, f->choices[i]) == 0) {
                *(int *)p = i;
                return;
            }
        }
        char list[128] = "";
        for (int i = 0; f->choices[i]; i++) {
            if (i)
                strncat(list, ", ", sizeof(list) - strlen(list) - 1);
            strncat(list, f->choices[i], sizeof(list) - strlen(list) - 1);
        }
        conf_err(c, e->line, "%s: '%s' is not one of: %s", e->key, v, list);
        return;
    }
}

static bool in_list(const char *s, const char *const *list)
{
    for (; list && *list; list++)
        if (strcmp(s, *list) == 0)
            return true;
    return false;
}

int conf_apply(conf_t *c, const conf_field_t *fields, void *dst,
               const char *const *open_sections)
{
    for (int i = 0; i < c->n; i++) {
        const conf_entry_t *e = &c->v[i];
        if (in_list(e->section, open_sections))
            continue;
        const conf_field_t *f = fields;
        while (f->key && !(strcmp(f->section, e->section) == 0 && strcmp(f->key, e->key) == 0))
            f++;
        if (!f->key) {
            if (e->section[0])
                conf_err(c, e->line, "unknown setting %s.%s", e->section, e->key);
            else
                conf_err(c, e->line, "%s: setting outside any [section]", e->key);
            continue;
        }
        store(c, e, f, dst);
    }
    return c->errors;
}
