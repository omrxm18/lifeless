#ifndef LIFELESS_TOKEN_H
#define LIFELESS_TOKEN_H

#include <stddef.h>

enum token_type {
  // specials
  TOK_EOF,
  TOK_ERROR,

  // literals and names
  TOK_INT,
  TOK_STRING,
  TOK_IDENT,

  // keywords
  TOK_DIE,
  TOK_TRY,
  TOK_RO,
  TOK_IF,
  TOK_ELSE,
  TOK_WHILE,
  TOK_RETURN,
  TOK_TRUE,
  TOK_FALSE,
  TOK_NONE,

  // punctuation
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_LBRACE,
  TOK_RBRACE,
  TOK_COMMA,
  TOK_SEMI,

  // operators
  TOK_ASSIGN, /* =  */
  TOK_EQ,
  TOK_NE, /* == != */
  TOK_LT,
  TOK_LE,
  TOK_GT,
  TOK_GE, /* < <= > >= */
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  TOK_PERCENT,
  TOK_AND,
  TOK_OR,
  TOK_NOT /* && || ! */
};

struct token {
  enum token_type type;
  const char *start; // first byte, points into the source buffer
  size_t length;     // bytes in the token
  size_t offset;     // byte offset of `start` from the buffer's beginning
  int line;          // 1-based
  int column;        // 1-based, counted in bytes
  const char *error; // message for TOK_ERROR (a static string), else NULL
};

#endif