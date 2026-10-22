#ifndef LIFELESS_DIAG_H
#define LIFELESS_DIAG_H

#include <stddef.h>
#include <stdio.h>

#include "arena.h"
#include "brain.h"

// Lets GCC check printf-style arguments. A no-op for other compilers.
#if defined(__GNUC__)
#define COMPLAINS_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define COMPLAINS_PRINTF(fmt, first)
#endif

struct complaints {
  struct complaints *next;
  int line;      // 1-based
  int column;    // 1-based, counted in bytes
  size_t offset; // byte offset where the problem starts
  size_t length; // bytes it covers (an editor underlines this range)
  const char *message;
};

struct complain_list {
  struct arena *arena;
  struct complaints *head;
  struct complaints *tail;
  size_t count;
};

void complain_init(struct complain_list *d, struct arena *arena);

// Record an error. `fmt` is printf-style
void complain_add(struct complain_list *d, int line, int column, size_t offset,
                  size_t length, const char *fmt, ...) COMPLAINS_PRINTF(6, 7);

// Print every complainnostic as "path:line:col: error: message"
void complain_print(const struct complain_list *d, const char *path, FILE *out);

/* Check a parsed program against the language rules (scoping, ro, die and
 * try) and record every violation in `diags`. Never runs the program. */
void check(const struct program *p, struct complain_list *complains);

#endif