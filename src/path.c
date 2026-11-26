#include "path.h"

#include "pagestore/types.h"

#include <stdlib.h>
#include <string.h>

char *ps_str_dup(const char *s)
{
    const usize n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

char *ps_str_join(const char *a, const char *b)
{
    const usize na = strlen(a);
    const usize nb = strlen(b);
    char *s = malloc(na + nb + 1);
    if (s != NULL) {
        memcpy(s, a, na);
        memcpy(s + na, b, nb + 1);
    }
    return s;
}

char *ps_path_parent(const char *path)
{
    usize n = strlen(path);
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    while (n > 0 && path[n - 1] != '/') {
        n--;
    }
    if (n == 0) {
        return ps_str_dup(".");
    }
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    char *s = malloc(n + 1);
    if (s != NULL) {
        memcpy(s, path, n);
        s[n] = '\0';
    }
    return s;
}
