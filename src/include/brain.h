#ifndef LIFELESS_BRAIN_H
#define LIFELESS_BRAIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tokens.h"

// A stretch of source text.
struct span {
  size_t offset; // byte offset of the first byte
  size_t length; // bytes covered
  int line;      // 1-based, of the first byte
  int column;    // 1-based, counted in bytes
};

struct span span_from_token(const struct token *t);

// From the start of `a` to the end of `b`
struct span span_join(struct span a, struct span b);

/* An identifier: a slice of the source buffer, not a copy. The source
 * buffer must outlive the tree. */
struct ident {
  const char *text;
  size_t len;
  struct span span;
};

struct func;
struct stmt;

enum expr_kind {
  EXPR_INT,
  EXPR_STR,
  EXPR_BOOL,
  EXPR_NONE,
  EXPR_VAR,
  EXPR_CALL,
  EXPR_UNARY,
  EXPR_BINARY
};

struct expr {
  enum expr_kind kind;
  struct span span;
  struct expr *next; // next argument, when this sits in a call's argument list
  union {
    int64_t int_value; // EXPR_INT
    struct {
      const char *data;
      size_t len;
    } str;            // EXPR_STR, decoded
    int bool_value;   // EXPR_BOOL
    struct ident var; // EXPR_VAR
    struct {
      struct ident callee;
      struct expr *args;
    } call; // EXPR_CALL
    struct {
      enum token_type op;
      struct expr *operand;
    } unary; // EXPR_UNARY
    struct {
      enum token_type op;
      struct expr *lhs, *rhs;
    } binary; // EXPR_BINARY
              // EXPR_NONE: no payload
  };
};

struct block {
  struct span span;   // from '{' to '}'
  struct stmt *stmts; // first statement; the rest follow through ->next
};

enum stmt_kind {
  STMT_IF,
  STMT_WHILE,
  STMT_RETURN,
  STMT_RO,
  STMT_ASSIGN,
  STMT_EXPR,
  STMT_FUNC
};

struct stmt {
  enum stmt_kind kind;
  struct span span;
  struct stmt *next;
  union {
    // STMT_IF
    struct {
      struct expr *cond;
      struct block *then_block;
      struct stmt *else_if;     // an STMT_IF, or NULL
      struct block *else_block; // or NULL; never both
    } if_stmt;
    struct {
      struct expr *cond;
      struct block *body;
    } while_stmt;
    struct {
      struct expr *value;
    } return_stmt; // value may be NULL
    struct {
      struct ident target;
      struct expr *value;
    } ro_stmt; // value NULL for "ro x;"
    struct {
      struct ident target;
      struct expr *value;
    } assign_stmt;
    struct {
      struct expr *expr;
    } expr_stmt;
    struct {
      struct func *func;
    } func_stmt;
  };
};

enum func_kind { FUNC_DIE, FUNC_TRY };

struct param {
  struct span span;
  struct ident name;
  int is_ro;
  struct param *next;
};

struct func {
  enum func_kind kind;
  struct span span;
  struct ident name;
  struct param *params;
  struct block *body;
};

struct program {
  struct stmt *stmts; // in source order; functions appear as STMT_FUNC
};

void brain_print(const struct program *p, FILE *out);

#endif