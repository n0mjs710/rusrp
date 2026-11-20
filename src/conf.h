#pragma once

/* Minimal config file reader: [section] headers, key = value lines, # comments.
 * Values are bare text; quotes are rejected. */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int   line;
    char  section[32];
    char *key;
    char *value;
} conf_entry_t;

typedef struct {
    const char   *path;
    conf_entry_t *v;
    int           n, cap;
    int           errors;
} conf_t;

typedef enum {
    CONF_STR,     /* char[size] */
    CONF_PORT,    /* uint16_t, 1–65535 */
    CONF_UINT,    /* unsigned int */
    CONF_INT,     /* int */
    CONF_FLOAT,   /* float */
    CONF_BOOL,    /* bool: true/false */
    CONF_CHOICE,  /* int (enum): index into choices */
} conf_type_t;

typedef struct {
    const char        *section;
    const char        *key;
    conf_type_t        type;
    size_t             off;
    size_t             size;      /* CONF_STR only */
    const char *const *choices;   /* CONF_CHOICE only; NULL-terminated */
} conf_field_t;

/* Returns -1 if the file can't be opened; syntax errors are counted in c->errors. */
int  conf_read(conf_t *c, const char *path);
void conf_free(conf_t *c);

/* Stores each entry matching a field into dst. Entries in a section listed in
 * open_sections (NULL-terminated, may be NULL) are left for the caller; any
 * other unmatched entry is an error. Returns c->errors. */
int  conf_apply(conf_t *c, const conf_field_t *fields, void *dst,
                const char *const *open_sections);

/* Reports "path:line: msg" on stderr and counts it; line 0 omits the line. */
void conf_err(conf_t *c, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

bool conf_parse_ll(const char *s, long long *out);
