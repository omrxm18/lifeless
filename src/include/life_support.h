#ifndef LIFELESS_INTERP_H
#define LIFELESS_INTERP_H

#include <stdio.h>

#include "arena.h"
#include "brain.h"

/* Run a program that has already passed parsing and checking. print writes
 * to `out`. Returns 0 on success. On a runtime error, prints
 * "path:line:col: runtime error: message" to stderr and returns 1.
 * All runtime memory (variables, strings) comes from `arena`. */
int run(const struct program *p, struct arena *arena, const char *path,
        FILE *out);

#endif