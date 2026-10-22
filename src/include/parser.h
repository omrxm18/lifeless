#ifndef LIFELESS_PARSER_H
#define LIFELESS_PARSER_H

#include <stddef.h>

#include "arena.h"
#include "brain.h"
#include "complaints.h"

/* Parse `len` bytes of source into a tree. The tree lives in `arena` and
 * points into `src`, so both must outlive any use of it. Syntax errors go
 * into `diags` (normally built on the same arena). Parsing carries on after
 * an error, so the result is never NULL, but a tree built from bad source
 * is incomplete. */
struct program *parse(const char *src, size_t len, struct arena *arena,
                      struct complain_list *diags);

#endif