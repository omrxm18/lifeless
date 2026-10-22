#ifndef LIFELESS_ARENA_H
#define LIFELESS_ARENA_H

#include <stddef.h>

struct arena_block;

struct arena {
  struct arena_block *head; // newest block first
};

void arena_init(struct arena *a);

/* Return `size` zeroed bytes, aligned for any type. Never returns NULL:
 * if memory runs out, arena.c reports it and exits. */
void *arena_alloc(struct arena *a, size_t size);

// Release every block at once
void arena_free(struct arena *a);

#endif