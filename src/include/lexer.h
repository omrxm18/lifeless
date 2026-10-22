#ifndef LIFELESS_LEXER_H
#define LIFELESS_LEXER_H

#include <stddef.h>

#include "tokens.h"

struct lexer {
  const char *src; // the whole source buffer, not owned by the lexer
  size_t len;      // its length in bytes
  size_t pos;      // offset of the next unread byte
  int line;        // line of `pos`, 1-based
  int column;      // column of `pos`, 1-based, in bytes
};

// Point the lexer at `src` (`len` bytes) and reset it to the start
void lexer_init(struct lexer *lx, const char *src, size_t len);

// Return the next token. After the end of input, it keeps returning TOK_EOF
struct token lexer_next(struct lexer *lx);

// A printable name for a token type, such as "DIE" or "PLUS" (a static string)
const char *token_type_name(enum token_type type);

/* read all of the file at `path` into a new NUL-terminated buffer
 * on success: return the buffer (the caller must free() it) and stores
 * the length, not counting the terminator, in *out_len.
 * on failure: returns NULL with errno set. */
char *file_read_all(const char *path, size_t *out_len);

#endif