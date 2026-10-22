// lifeless - lexer.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

void lexer_init(struct lexer *lx, const char *src, size_t len) {
  lx->src = src;
  lx->len = len;
  lx->pos = 0;
  lx->line = 1;
  lx->column = 1;
}

static int at_end(const struct lexer *lx) { return lx->pos >= lx->len; }

// The byte `ahead` places past the current one, or '\0' past the end
static char peek(const struct lexer *lx, size_t ahead) {
  size_t i = lx->pos + ahead;
  return i < lx->len ? lx->src[i] : '\0';
}

/* Consume one byte, keeping line and column in step. Call only when
 * !at_end(lx). */
static char advance(struct lexer *lx) {
  char c = lx->src[lx->pos++];

  if (c == '\n') {
    lx->line++;
    lx->column = 1;
  } else {
    lx->column++;
  }
  return c;
}

/* `begin` is a copy of the lexer taken before the token's first byte was
 * consumed. The token spans from there to the lexer's current position. */
static struct token make_token(const struct lexer *lx,
                               const struct lexer *begin, enum token_type type,
                               const char *error) {
  struct token t;

  t.type = type;
  t.start = lx->src + begin->pos;
  t.length = lx->pos - begin->pos;
  t.offset = begin->pos;
  t.line = begin->line;
  t.column = begin->column;
  t.error = error;
  return t;
}

/* Skip whitespace, // comments and block comments. Returns 0 normally. If a
 * block comment is never closed, consumes the rest of the input, stores an
 * error token in *err, and returns 1. */
static int skip_trivia(struct lexer *lx, struct token *err) {
  for (;;) {
    char c = peek(lx, 0);

    if (at_end(lx))
      return 0;

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance(lx);
    } else if (c == '/' && peek(lx, 1) == '/') {
      while (!at_end(lx) && peek(lx, 0) != '\n')
        advance(lx);
    } else if (c == '/' && peek(lx, 1) == '*') {
      struct lexer begin = *lx;

      advance(lx);
      advance(lx);
      for (;;) {
        if (at_end(lx)) {
          *err = make_token(lx, &begin, TOK_ERROR, "unterminated comment");
          return 1;
        }
        if (peek(lx, 0) == '*' && peek(lx, 1) == '/') {
          advance(lx);
          advance(lx);
          break;
        }
        advance(lx);
      }
    } else {
      return 0;
    }
  }
}

/* Plain ASCII tests. <ctype.h> is avoided on purpose: it depends on the
 * locale, and passing it a negative char is undefined behaviour. */
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

static const struct {
  const char *text;
  enum token_type type;
} keywords[] = {
    {"die", TOK_DIE},       {"try", TOK_TRY},   {"ro", TOK_RO},
    {"if", TOK_IF},         {"else", TOK_ELSE}, {"while", TOK_WHILE},
    {"return", TOK_RETURN}, {"true", TOK_TRUE}, {"false", TOK_FALSE},
    {"none", TOK_NONE},
};

// TOK_IDENT unless the n bytes at s spell a keyword
static enum token_type keyword_type(const char *s, size_t n) {
  for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
    const char *kw = keywords[i].text;

    if (strlen(kw) == n && memcmp(kw, s, n) == 0)
      return keywords[i].type;
  }
  return TOK_IDENT;
}

/* An identifier or keyword. The caller has already checked that the
 * current byte satisfies is_ident_start(). */
static struct token lex_word(struct lexer *lx) {
  struct lexer begin = *lx;
  struct token t;

  while (is_ident_char(peek(lx, 0)))
    advance(lx);

  t = make_token(lx, &begin, TOK_IDENT, NULL);
  t.type = keyword_type(t.start, t.length);
  return t;
}

// A decimal integer. The caller has already checked for a leading digit
static struct token lex_number(struct lexer *lx) {
  struct lexer begin = *lx;

  while (is_digit(peek(lx, 0)))
    advance(lx);

  /* "12abc" or "0x1F": digits glued to letters are one bad token, not
   * an INT followed by an IDENT. */
  if (is_ident_char(peek(lx, 0))) {
    while (is_ident_char(peek(lx, 0)))
      advance(lx);
    return make_token(lx, &begin, TOK_ERROR, "invalid number");
  }
  return make_token(lx, &begin, TOK_INT, NULL);
}

/* A string literal. The caller has checked that the current byte is '"'.
 * The token includes both quotes. Escapes are not decoded here (a later
 * stage does that). At most one error is reported per string. */
static struct token lex_string(struct lexer *lx) {
  struct lexer begin = *lx;
  const char *error = NULL;

  advance(lx);

  for (;;) {
    char c = peek(lx, 0);

    if (at_end(lx) || c == '\n') {
      /* No closing quote on this line. The newline is left for
       * skip_trivia, so line counting stays in one place. */
      return make_token(lx, &begin, TOK_ERROR, "unterminated string");
    }

    advance(lx);

    if (c == '"')
      break;

    if (c == '\0') {
      if (!error)
        error = "invalid character in string";
    } else if (c == '\\') {
      char e = peek(lx, 0);

      if (at_end(lx) || e == '\n')
        continue;
      advance(lx);
      if (e != 'n' && e != 't' && e != '"' && e != '\\' && !error)
        error = "invalid escape sequence";
    }
  }

  return make_token(lx, &begin, error ? TOK_ERROR : TOK_STRING, error);
}

static int match(struct lexer *lx, char expected) {
  if (peek(lx, 0) != expected)
    return 0;
  advance(lx);
  return 1;
}

struct token lexer_next(struct lexer *lx) {
  struct token err;
  struct lexer begin;
  char c;

  if (skip_trivia(lx, &err))
    return err;

  begin = *lx;

  if (at_end(lx))
    return make_token(lx, &begin, TOK_EOF, NULL);

  c = peek(lx, 0);

  if (is_ident_start(c))
    return lex_word(lx);
  if (is_digit(c))
    return lex_number(lx);
  if (c == '"')
    return lex_string(lx);

  advance(lx);

  switch (c) {
  case '(':
    return make_token(lx, &begin, TOK_LPAREN, NULL);
  case ')':
    return make_token(lx, &begin, TOK_RPAREN, NULL);
  case '{':
    return make_token(lx, &begin, TOK_LBRACE, NULL);
  case '}':
    return make_token(lx, &begin, TOK_RBRACE, NULL);
  case ',':
    return make_token(lx, &begin, TOK_COMMA, NULL);
  case ';':
    return make_token(lx, &begin, TOK_SEMI, NULL);
  case '+':
    return make_token(lx, &begin, TOK_PLUS, NULL);
  case '-':
    return make_token(lx, &begin, TOK_MINUS, NULL);
  case '*':
    return make_token(lx, &begin, TOK_STAR, NULL);
  case '/':
    return make_token(lx, &begin, TOK_SLASH, NULL);
  case '%':
    return make_token(lx, &begin, TOK_PERCENT, NULL);

  case '=':
    return make_token(lx, &begin, match(lx, '=') ? TOK_EQ : TOK_ASSIGN, NULL);
  case '!':
    return make_token(lx, &begin, match(lx, '=') ? TOK_NE : TOK_NOT, NULL);
  case '<':
    return make_token(lx, &begin, match(lx, '=') ? TOK_LE : TOK_LT, NULL);
  case '>':
    return make_token(lx, &begin, match(lx, '=') ? TOK_GE : TOK_GT, NULL);

  case '&':
    if (match(lx, '&'))
      return make_token(lx, &begin, TOK_AND, NULL);
    return make_token(lx, &begin, TOK_ERROR,
                      "unexpected '&' (did you mean '&&'?)");
  case '|':
    if (match(lx, '|'))
      return make_token(lx, &begin, TOK_OR, NULL);
    return make_token(lx, &begin, TOK_ERROR,
                      "unexpected '|' (did you mean '||'?)");
  }

  if ((unsigned char)c >= 0x80) {
    /* One error per character, not per byte: swallow the rest of a
     * UTF-8 sequence (continuation bytes look like 10xxxxxx). */
    while (((unsigned char)peek(lx, 0) & 0xC0) == 0x80)
      advance(lx);
    return make_token(lx, &begin, TOK_ERROR,
                      "non-ASCII character outside a string");
  }

  return make_token(lx, &begin, TOK_ERROR, "unexpected character");
}

const char *token_type_name(enum token_type type) {
  switch (type) {
  case TOK_EOF:
    return "EOF";
  case TOK_ERROR:
    return "ERROR";
  case TOK_INT:
    return "INT";
  case TOK_STRING:
    return "STRING";
  case TOK_IDENT:
    return "IDENT";
  case TOK_DIE:
    return "DIE";
  case TOK_TRY:
    return "TRY";
  case TOK_RO:
    return "RO";
  case TOK_IF:
    return "IF";
  case TOK_ELSE:
    return "ELSE";
  case TOK_WHILE:
    return "WHILE";
  case TOK_RETURN:
    return "RETURN";
  case TOK_TRUE:
    return "TRUE";
  case TOK_FALSE:
    return "FALSE";
  case TOK_NONE:
    return "NONE";
  case TOK_LPAREN:
    return "LPAREN";
  case TOK_RPAREN:
    return "RPAREN";
  case TOK_LBRACE:
    return "LBRACE";
  case TOK_RBRACE:
    return "RBRACE";
  case TOK_COMMA:
    return "COMMA";
  case TOK_SEMI:
    return "SEMI";
  case TOK_ASSIGN:
    return "ASSIGN";
  case TOK_EQ:
    return "EQ";
  case TOK_NE:
    return "NE";
  case TOK_LT:
    return "LT";
  case TOK_LE:
    return "LE";
  case TOK_GT:
    return "GT";
  case TOK_GE:
    return "GE";
  case TOK_PLUS:
    return "PLUS";
  case TOK_MINUS:
    return "MINUS";
  case TOK_STAR:
    return "STAR";
  case TOK_SLASH:
    return "SLASH";
  case TOK_PERCENT:
    return "PERCENT";
  case TOK_AND:
    return "AND";
  case TOK_OR:
    return "OR";
  case TOK_NOT:
    return "NOT";
  }
  return "?";
}

char *file_read_all(const char *path, size_t *out_len) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fclose(fp);
    return NULL;
  }

  for (;;) {
    if (len + 1 == cap) {
      size_t ncap = cap * 2;
      char *nbuf = realloc(buf, ncap);
      if (!nbuf) {
        free(buf);
        fclose(fp);
        errno = ENOMEM;
        return NULL;
      }
      buf = nbuf;
      cap = ncap;
    }
    size_t n = fread(buf + len, 1, cap - len - 1, fp);
    if (n == 0)
      break;
    len += n;
  }

  if (ferror(fp)) {
    int saved = errno;
    free(buf);
    fclose(fp);
    errno = saved;
    return NULL;
  }

  fclose(fp);
  buf[len] = '\0';
  if (out_len)
    *out_len = len;
  return buf;
}