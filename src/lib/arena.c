// lifeless - arena.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include "arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ARENA_BLOCK_SIZE 16384
#define ARENA_ALIGN (_Alignof(max_align_t))

struct arena_block {
  struct arena_block *next;
  size_t used; // bytes of data[] handed out, including padding
  size_t cap;  // size of data[]
  unsigned char data[];
};

void arena_init(struct arena *a) { a->head = NULL; }

static struct arena_block *new_block(size_t cap) {
  struct arena_block *b = NULL;

  if (cap <= SIZE_MAX - sizeof *b)
    b = calloc(1, sizeof *b + cap);
  if (!b) {
    fputs("lifeless: out of memory\n", stderr);
    exit(2);
  }
  b->cap = cap;
  return b;
}

// Carve `size` aligned bytes out of `b`, or NULL if it does not fit
static void *block_alloc(struct arena_block *b, size_t size) {
  uintptr_t base = (uintptr_t)b->data;
  uintptr_t p =
      (base + b->used + ARENA_ALIGN - 1) & ~(uintptr_t)(ARENA_ALIGN - 1);
  size_t off = (size_t)(p - base);

  if (off > b->cap || size > b->cap - off)
    return NULL;
  b->used = off + size;
  return (void *)p;
}

void *arena_alloc(struct arena *a, size_t size) {
  void *p = a->head ? block_alloc(a->head, size) : NULL;
  size_t cap;
  struct arena_block *b;

  if (p)
    return p;

  /* Current block is full (or there is none yet): start a new one. A
   * request bigger than a normal block gets a block of its own. */
  cap = size + ARENA_ALIGN; // room for worst-case padding
  if (cap < size)
    cap = SIZE_MAX; // overflow: new_block will refuse
  if (cap < ARENA_BLOCK_SIZE)
    cap = ARENA_BLOCK_SIZE;

  b = new_block(cap);
  b->next = a->head;
  a->head = b;
  return block_alloc(b, size);
}

void arena_free(struct arena *a) {
  struct arena_block *b = a->head;

  while (b) {
    struct arena_block *next = b->next;

    free(b);
    b = next;
  }
  a->head = NULL;
}