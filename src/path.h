#pragma once

/*
 * String and path helpers shared by the file backend and the WAL. Each
 * returns a new string for the caller to free, or NULL if out of memory.
 */

char *ps_str_dup(const char *s);
char *ps_str_join(const char *a, const char *b);

/* For example, "/a/b" gives "/a", "/a" gives "/", and "a" gives ".". */
char *ps_path_parent(const char *path);
