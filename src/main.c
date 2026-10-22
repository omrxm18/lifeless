// lifeless - main.c
// Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "brain.h"
#include "complaints.h"
#include "lexer.h"
#include "life_support.h"
#include "parser.h"
#include "version.h"

// Exit codes: 0 success, 1 the program has errors, 2 bad usage or I/O
#define EXIT_PROGRAM_ERROR 1
#define EXIT_USAGE 2

static void print_usage(FILE *out) {
  fprintf(out,
          "usage: lifeless [options] file.lfs\n"
          "\n"
          "options:\n"
          "  --tokens     print the token stream instead of running\n"
          "  --help       show this help and exit\n"
          "  --version    show the version and exit\n"
          "  --ast        print the syntax tree instead of running\n"
          "  --check      check the program for errors without running it\n");
}

// Print `n` bytes as a double-quoted string on one line.
static void print_escaped(const char *s, size_t n) {
  putchar('"');
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];

    if (c == '"' || c == '\\')
      printf("\\%c", c);
    else if (c == '\n')
      fputs("\\n", stdout);
    else if (c == '\t')
      fputs("\\t", stdout);
    else if (c == '\r')
      fputs("\\r", stdout);
    else if (c < 0x20 || c == 0x7f)
      printf("\\x%02x", c);
    else
      putchar(c);
  }
  putchar('"');
}

/* One line per token on stdout; lexical errors also go to stderr as
 * file:line:col: error: message. Returns the number of error tokens. */
static int dump_tokens(const char *path, const char *src, size_t len) {
  struct lexer lx;
  int errors = 0;

  lexer_init(&lx, src, len);

  for (;;) {
    struct token t = lexer_next(&lx);

    printf("%d:%d %s ", t.line, t.column, token_type_name(t.type));
    print_escaped(t.start, t.length);
    putchar('\n');

    if (t.type == TOK_ERROR) {
      fprintf(stderr, "%s:%d:%d: error: %s\n", path, t.line, t.column, t.error);
      errors++;
    }
    if (t.type == TOK_EOF)
      break;
  }
  return errors;
}

// Parse, print the tree to stdout and any syntax errors to stderr. Returns the
// number of errors.
static int dump_ast(const char *path, const char *src, size_t len) {
  struct arena arena;
  struct complain_list complains;
  struct program *prog;
  int errors;

  arena_init(&arena);
  complain_init(&complains, &arena);

  prog = parse(src, len, &arena, &complains);
  brain_print(prog, stdout);
  complain_print(&complains, path, stderr);

  errors = (int)complains.count;
  arena_free(&arena);
  return errors;
}

/* Parse, then check. Prints every diagnostic to stderr and nothing else.
 * Returns the number of errors. The checker is skipped when the parser
 * already found errors, since a tree built from broken source would only
 * produce misleading checker errors. */
static int check_program(const char *path, const char *src, size_t len) {
  struct arena arena;
  struct complain_list complains;
  struct program *prog;
  int errors;

  arena_init(&arena);
  complain_init(&complains, &arena);

  prog = parse(src, len, &arena, &complains);
  if (complains.count == 0)
    check(prog, &complains);
  complain_print(&complains, path, stderr);

  errors = (int)complains.count;
  arena_free(&arena);
  return errors;
}

/* Parse, check, and run. Returns 0, or 1 if parsing, checking, or the run
 * itself failed. Nothing runs when parsing or checking found errors. */
static int run_program(const char *path, const char *src, size_t len) {
  struct arena arena;
  struct complain_list complains;
  struct program *prog;
  int status;

  arena_init(&arena);
  complain_init(&complains, &arena);

  prog = parse(src, len, &arena, &complains);
  if (complains.count == 0)
    check(prog, &complains);

  if (complains.count > 0) {
    complain_print(&complains, path, stderr);
    status = EXIT_PROGRAM_ERROR;
  } else {
    status = run(prog, &arena, path, stdout);
  }

  arena_free(&arena);
  return status;
}

int main(int argc, char **argv) {
  const char *path = NULL;
  int tokens = 0;
  int ast = 0;
  int check_only = 0;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "--help") == 0) {
      print_usage(stdout);
      return EXIT_SUCCESS;
    }
    if (strcmp(arg, "--version") == 0) {
      printf("lifeless %s\n", LIFELESS_VERSION);
      return EXIT_SUCCESS;
    }
    if (strcmp(arg, "--tokens") == 0) {
      tokens = 1;
      continue;
    }
    if (strcmp(arg, "--ast") == 0) {
      ast = 1;
      continue;
    }
    if (strcmp(arg, "--check") == 0) {
      check_only = 1;
      continue;
    }
    if (arg[0] == '-') {
      fprintf(stderr, "lifeless: unknown option '%s'\n", arg);
      print_usage(stderr);
      return EXIT_USAGE;
    }
    if (path) {
      fprintf(stderr, "lifeless: only one input file is allowed\n");
      return EXIT_USAGE;
    }
    path = arg;
  }

  if (!path) {
    print_usage(stderr);
    return EXIT_USAGE;
  }

  size_t len = 0;
  char *src = file_read_all(path, &len);
  if (!src) {
    fprintf(stderr, "lifeless: cannot read '%s': %s\n", path, strerror(errno));
    return EXIT_USAGE;
  }

  int status = EXIT_SUCCESS;

  if (tokens) {
    if (dump_tokens(path, src, len) > 0)
      status = EXIT_PROGRAM_ERROR;
  } else if (ast) {
    if (dump_ast(path, src, len) > 0)
      status = EXIT_PROGRAM_ERROR;
  } else if (check_only) {
    if (check_program(path, src, len) > 0)
      status = EXIT_PROGRAM_ERROR;
  } else {
    run_program(path, src, len);
  }

  free(src);
  return status;
}