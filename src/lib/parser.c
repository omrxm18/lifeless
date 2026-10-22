// lifeless - parser.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include "parser.h"

#include "complaints.h"
#include "lexer.h"

struct parser {
  struct lexer lx;
  struct arena *arena;
  struct complain_list *complains;

  struct token cur;  // the token being looked at
  struct token next; // one token further ahead
  struct token prev; // the token consumed last, used for spans

  int depth; // statement and expression nesting so far
};

/* Record an error at token `t`. An error at a TOK_ERROR token is skipped:
 * the lexer already reported that spot, and a second message about it
 * would only be noise. */
#define report(p, t, ...)                                                      \
  do {                                                                         \
    const struct token *t_ = (t);                                              \
    if (t_->type != TOK_ERROR)                                                 \
      complain_add((p)->complains, t_->line, t_->column, t_->offset,           \
                   t_->length, __VA_ARGS__);                                   \
  } while (0)

/* Next token from the lexer. Each lexer error is reported here, exactly
 * once, as the token is fetched. The error token itself is still handed on,
 * so the parser sees "something unexpected" at that spot. */
static struct token fetch(struct parser *p) {
  struct token t = lexer_next(&p->lx);

  if (t.type == TOK_ERROR)
    complain_add(p->complains, t.line, t.column, t.offset, t.length, "%s",
                 t.error);
  return t;
}

static void advance(struct parser *p) {
  p->prev = p->cur;
  p->cur = p->next;
  p->next = fetch(p);
}

static int checkparse(const struct parser *p, enum token_type type) {
  return p->cur.type == type;
}

// Consume the current token if it has this type.
static int match(struct parser *p, enum token_type type) {
  if (!checkparse(p, type))
    return 0;
  advance(p);
  return 1;
}

// Report "expected <what>, found <the current token>".
static void error_expected(struct parser *p, const char *what) {
  if (checkparse(p, TOK_EOF))
    report(p, &p->cur, "expected %s, found end of file", what);
  else
    report(p, &p->cur, "expected %s, found '%.*s'", what, (int)p->cur.length,
           p->cur.start);
}

/* Consume a token of this type, or report an error and return 0. `what`
 * names it for the message, for example "';'" */
static int expect(struct parser *p, enum token_type type, const char *what) {
  if (match(p, type))
    return 1;
  error_expected(p, what);
  return 0;
}

static void parser_init(struct parser *p, const char *src, size_t len,
                        struct arena *arena, struct complain_list *complains) {
  lexer_init(&p->lx, src, len);
  p->arena = arena;
  p->complains = complains;
  p->depth = 0;
  p->prev = (struct token){.type = TOK_EOF};
  p->cur = fetch(p);
  p->next = fetch(p);
}

/* After a syntax error, skip ahead to a place where a new statement can
 * plausibly start: just past a ';', or at a '}' or a statement keyword. */
static void synchronize(struct parser *p) {
  while (!checkparse(p, TOK_EOF)) {
    if (p->prev.type == TOK_SEMI)
      return;

    switch (p->cur.type) {
    case TOK_RBRACE:
    case TOK_DIE:
    case TOK_TRY:
    case TOK_RO:
    case TOK_IF:
    case TOK_WHILE:
    case TOK_RETURN:
      return;
    default:
      break;
    }
    advance(p);
  }
}

#define MAX_NESTING 200

/* Call before descending one level of nesting; if it returns 0 (after
 * reporting an error), do not descend. Pair every success with leave(). */
static int enter(struct parser *p) {
  if (p->depth >= MAX_NESTING) {
    report(p, &p->cur, "nesting is too deep");
    return 0;
  }
  p->depth++;
  return 1;
}

static void leave(struct parser *p) { p->depth--; }

static struct ident ident_from_token(const struct token *t) {
  struct ident id;

  id.text = t->start;
  id.len = t->length;
  id.span = span_from_token(t);
  return id;
}

static struct expr *new_expr(struct parser *p, enum expr_kind kind,
                             struct span span) {
  struct expr *e = arena_alloc(p->arena, sizeof *e);

  e->kind = kind;
  e->span = span;
  return e;
}

// Decimal digits to int64. Returns 0 if the value does not fit
static int parse_int(const struct token *t, int64_t *out) {
  const uint64_t max = INT64_MAX;
  uint64_t v = 0;

  for (size_t i = 0; i < t->length; i++) {
    uint64_t d = (uint64_t)(t->start[i] - '0');

    if (v > (max - d) / 10)
      return 0;
    v = v * 10 + d;
  }
  *out = (int64_t)v;
  return 1;
}

/* Decode a string token (quotes included) into arena memory. The lexer has
 * already checked every escape, so only the four legal ones occur. */
static const char *decode_string(struct parser *p, const struct token *t,
                                 size_t *out_len) {
  const char *s = t->start + 1;
  size_t n = t->length - 2;
  char *buf = arena_alloc(p->arena, n + 1); /* decoding never grows the text */
  size_t len = 0;

  for (size_t i = 0; i < n; i++) {
    char c = s[i];

    if (c == '\\' && i + 1 < n) {
      i++;
      switch (s[i]) {
      case 'n':
        c = '\n';
        break;
      case 't':
        c = '\t';
        break;
      default:
        c = s[i];
        break;
      }
    }
    buf[len++] = c;
  }
  buf[len] = '\0';
  *out_len = len;
  return buf;
}

static struct expr *parse_expr(struct parser *p);

static struct expr *parse_call(struct parser *p, const struct token *name_tok,
                               struct ident name) {
  struct expr *head = NULL, *tail = NULL, *e;

  advance(p);

  if (!checkparse(p, TOK_RPAREN)) {
    do {
      struct expr *arg = parse_expr(p);

      if (!arg)
        return NULL;
      if (tail)
        tail->next = arg;
      else
        head = arg;
      tail = arg;
    } while (match(p, TOK_COMMA));
  }
  if (!expect(p, TOK_RPAREN, "')'"))
    return NULL;

  e = new_expr(p, EXPR_CALL,
               span_join(span_from_token(name_tok), span_from_token(&p->prev)));
  e->call.callee = name;
  e->call.args = head;
  return e;
}

static struct expr *parse_primary(struct parser *p) {
  struct token t = p->cur;
  struct expr *e;

  switch (t.type) {
  case TOK_INT: {
    int64_t v;

    advance(p);
    if (!parse_int(&t, &v)) {
      report(p, &t, "integer literal is too large");
      return NULL;
    }
    e = new_expr(p, EXPR_INT, span_from_token(&t));
    e->int_value = v;
    return e;
  }
  case TOK_STRING: {
    size_t len;
    const char *data = decode_string(p, &t, &len);

    advance(p);
    e = new_expr(p, EXPR_STR, span_from_token(&t));
    e->str.data = data;
    e->str.len = len;
    return e;
  }
  case TOK_TRUE:
  case TOK_FALSE:
    advance(p);
    e = new_expr(p, EXPR_BOOL, span_from_token(&t));
    e->bool_value = (t.type == TOK_TRUE);
    return e;
  case TOK_NONE:
    advance(p);
    return new_expr(p, EXPR_NONE, span_from_token(&t));
  case TOK_IDENT: {
    struct ident name = ident_from_token(&t);

    advance(p);
    if (checkparse(p, TOK_LPAREN))
      return parse_call(p, &t, name);
    e = new_expr(p, EXPR_VAR, span_from_token(&t));
    e->var = name;
    return e;
  }
  case TOK_LPAREN: {
    struct expr *inner;

    advance(p);
    inner = parse_expr(p);
    if (!inner || !expect(p, TOK_RPAREN, "')'"))
      return NULL;
    return inner; // parentheses leave no node
  }
  default:
    error_expected(p, "an expression"); // token not consumed
    return NULL;
  }
}

static struct expr *parse_unary(struct parser *p) {
  struct expr *e = NULL;

  if (!enter(p))
    return NULL;

  if (checkparse(p, TOK_MINUS) || checkparse(p, TOK_NOT)) {
    struct token op = p->cur;
    struct expr *operand;

    advance(p);
    operand = parse_unary(p);
    if (operand) {
      e = new_expr(p, EXPR_UNARY,
                   span_join(span_from_token(&op), operand->span));
      e->unary.op = op.type;
      e->unary.operand = operand;
    }
  } else {
    e = parse_primary(p);
  }

  leave(p);
  return e;
}

/* Binding strength of a binary operator; 0 means "not a binary operator".
 * Higher binds tighter. This is C's precedence, minus the bitwise levels. */
static int binary_prec(enum token_type t) {
  switch (t) {
  case TOK_OR:
    return 1;
  case TOK_AND:
    return 2;
  case TOK_EQ:
  case TOK_NE:
    return 3;
  case TOK_LT:
  case TOK_LE:
  case TOK_GT:
  case TOK_GE:
    return 4;
  case TOK_PLUS:
  case TOK_MINUS:
    return 5;
  case TOK_STAR:
  case TOK_SLASH:
  case TOK_PERCENT:
    return 6;
  default:
    return 0;
  }
}

/* Parse an expression, consuming only binary operators with at least
 * `min_prec` strength */
static struct expr *parse_expr_prec(struct parser *p, int min_prec) {
  struct expr *lhs = parse_unary(p);

  if (!lhs)
    return NULL;

  for (;;) {
    int prec = binary_prec(p->cur.type);
    enum token_type op;
    struct expr *rhs, *e;

    if (prec == 0 || prec < min_prec)
      break;

    op = p->cur.type;
    advance(p);
    rhs = parse_expr_prec(p, prec + 1); // +1: left-associative
    if (!rhs)
      return NULL;

    e = new_expr(p, EXPR_BINARY, span_join(lhs->span, rhs->span));
    e->binary.op = op;
    e->binary.lhs = lhs;
    e->binary.rhs = rhs;
    lhs = e;
  }
  return lhs;
}

static struct expr *parse_expr(struct parser *p) {
  return parse_expr_prec(p, 1);
}

static struct stmt *parse_stmt(struct parser *p);
static struct block *parse_block(struct parser *p);
static struct stmt *parse_func(struct parser *p); // defined with the top level

static struct stmt *new_stmt(struct parser *p, enum stmt_kind kind,
                             struct span span) {
  struct stmt *s = arena_alloc(p->arena, sizeof *s);

  s->kind = kind;
  s->span = span;
  return s;
}

// The span from `first` to the token consumed last
static struct span span_to_prev(const struct parser *p,
                                const struct token *first) {
  return span_join(span_from_token(first), span_from_token(&p->prev));
}

/* Parse statements until the closing '}' (when in a block) or the end of
 * input. A statement that fails is recorded and skipped, and the loop
 * carries on, so one bad statement does not hide the ones after it. */
static struct stmt *parse_stmt_list(struct parser *p, int in_block) {
  struct stmt *head = NULL, *tail = NULL;

  while (!checkparse(p, TOK_EOF)) {
    const char *before = p->cur.start;
    struct stmt *s;

    if (checkparse(p, TOK_RBRACE)) {
      if (in_block)
        break;
      report(p, &p->cur, "unmatched '}'");
      advance(p);
      continue;
    }

    s = parse_stmt(p);
    if (s) {
      if (tail)
        tail->next = s;
      else
        head = s;
      tail = s;
    } else {
      synchronize(p);
      // Guarantee progress: if nothing was consumed, step over the offending
      // token.
      if (p->cur.start == before && !checkparse(p, TOK_EOF) &&
          !checkparse(p, TOK_RBRACE))
        advance(p);
    }
  }
  return head;
}

static struct block *parse_block(struct parser *p) {
  struct token open = p->cur;
  struct stmt *stmts;
  struct block *b;

  if (!expect(p, TOK_LBRACE, "'{'"))
    return NULL;
  if (!enter(p))
    return NULL;
  stmts = parse_stmt_list(p, 1);
  leave(p);

  if (!expect(p, TOK_RBRACE, "'}'"))
    return NULL;

  b = arena_alloc(p->arena, sizeof *b);
  b->span = span_to_prev(p, &open);
  b->stmts = stmts;
  return b;
}

static struct stmt *parse_assign(struct parser *p) {
  struct token name = p->cur;
  struct expr *value;
  struct stmt *s;

  advance(p);
  advance(p);
  value = parse_expr(p);
  if (!value || !expect(p, TOK_SEMI, "';'"))
    return NULL;

  s = new_stmt(p, STMT_ASSIGN, span_to_prev(p, &name));
  s->assign_stmt.target = ident_from_token(&name);
  s->assign_stmt.value = value;
  return s;
}

static struct stmt *parse_expr_stmt(struct parser *p) {
  struct token first = p->cur;
  struct expr *e = parse_expr(p);
  struct stmt *s;

  if (!e || !expect(p, TOK_SEMI, "';'"))
    return NULL;

  s = new_stmt(p, STMT_EXPR, span_to_prev(p, &first));
  s->expr_stmt.expr = e;
  return s;
}

static struct stmt *parse_return(struct parser *p) {
  struct token kw = p->cur;
  struct expr *value = NULL;
  struct stmt *s;

  advance(p);
  if (!checkparse(p, TOK_SEMI)) {
    value = parse_expr(p);
    if (!value)
      return NULL;
  }
  if (!expect(p, TOK_SEMI, "';'"))
    return NULL;

  s = new_stmt(p, STMT_RETURN, span_to_prev(p, &kw));
  s->return_stmt.value = value;
  return s;
}

static struct stmt *parse_ro(struct parser *p) {
  struct token kw = p->cur;
  struct token name;
  struct expr *value = NULL;
  struct stmt *s;

  advance(p);
  name = p->cur;
  if (!expect(p, TOK_IDENT, "a variable name"))
    return NULL;
  if (match(p, TOK_ASSIGN)) {
    value = parse_expr(p);
    if (!value)
      return NULL;
  }
  if (!expect(p, TOK_SEMI, "';'"))
    return NULL;

  s = new_stmt(p, STMT_RO, span_to_prev(p, &kw));
  s->ro_stmt.target = ident_from_token(&name);
  s->ro_stmt.value = value;
  return s;
}

static struct stmt *parse_while(struct parser *p) {
  struct token kw = p->cur;
  struct expr *cond;
  struct block *body;
  struct stmt *s;

  advance(p);
  if (!expect(p, TOK_LPAREN, "'(' after 'while'"))
    return NULL;
  cond = parse_expr(p);
  if (!cond || !expect(p, TOK_RPAREN, "')'"))
    return NULL;
  body = parse_block(p);
  if (!body)
    return NULL;

  s = new_stmt(p, STMT_WHILE, span_join(span_from_token(&kw), body->span));
  s->while_stmt.cond = cond;
  s->while_stmt.body = body;
  return s;
}

static struct stmt *parse_if(struct parser *p) {
  struct token kw = p->cur;
  struct expr *cond;
  struct block *then_block, *else_block = NULL;
  struct stmt *else_if = NULL, *s;
  struct span end;

  advance(p);
  if (!expect(p, TOK_LPAREN, "'(' after 'if'"))
    return NULL;
  cond = parse_expr(p);
  if (!cond || !expect(p, TOK_RPAREN, "')'"))
    return NULL;
  then_block = parse_block(p);
  if (!then_block)
    return NULL;
  end = then_block->span;

  if (match(p, TOK_ELSE)) {
    if (checkparse(p, TOK_IF)) {
      if (!enter(p))
        return NULL;
      else_if = parse_if(p);
      leave(p);
      if (!else_if)
        return NULL;
      end = else_if->span;
    } else if (checkparse(p, TOK_LBRACE)) {
      else_block = parse_block(p);
      if (!else_block)
        return NULL;
      end = else_block->span;
    } else {
      error_expected(p, "'if' or '{' after 'else'");
      return NULL;
    }
  }

  s = new_stmt(p, STMT_IF, span_join(span_from_token(&kw), end));
  s->if_stmt.cond = cond;
  s->if_stmt.then_block = then_block;
  s->if_stmt.else_if = else_if;
  s->if_stmt.else_block = else_block;
  return s;
}

static struct stmt *parse_stmt(struct parser *p) {
  switch (p->cur.type) {
  case TOK_IF:
    return parse_if(p);
  case TOK_WHILE:
    return parse_while(p);
  case TOK_RETURN:
    return parse_return(p);
  case TOK_RO:
    return parse_ro(p);
  case TOK_DIE:
  case TOK_TRY:
    return parse_func(p);
  case TOK_IDENT:
    // The one place that needs two tokens of lookahead: "x = ..." is an
    // assignment, anything else is an expression.
    if (p->next.type == TOK_ASSIGN)
      return parse_assign(p);
    return parse_expr_stmt(p);
  default:
    return parse_expr_stmt(p);
  }
}

static struct stmt *parse_func(struct parser *p) {
  struct token kw = p->cur;
  struct token name;
  struct param *params = NULL, *ptail = NULL;
  struct block *body;
  struct func *f;
  struct stmt *s;

  /* Functions belong at top level. Complain, but keep parsing the
   * function so errors inside it are still found. */
  if (p->depth > 0)
    report(p, &kw, "functions can only be declared at top level");

  advance(p); /* 'die' or 'try' */
  name = p->cur;
  if (!expect(p, TOK_IDENT, "a function name"))
    return NULL;
  if (!expect(p, TOK_LPAREN, "'('"))
    return NULL;

  if (!checkparse(p, TOK_RPAREN)) {
    do {
      struct token first = p->cur;
      int is_ro = match(p, TOK_RO);
      struct token pname = p->cur;
      struct param *pm;

      if (!expect(p, TOK_IDENT, "a parameter name"))
        return NULL;

      pm = arena_alloc(p->arena, sizeof *pm);
      pm->span = span_to_prev(p, &first);
      pm->name = ident_from_token(&pname);
      pm->is_ro = is_ro;
      if (ptail)
        ptail->next = pm;
      else
        params = pm;
      ptail = pm;
    } while (match(p, TOK_COMMA));
  }
  if (!expect(p, TOK_RPAREN, "')'"))
    return NULL;

  body = parse_block(p);
  if (!body)
    return NULL;

  f = arena_alloc(p->arena, sizeof *f);
  f->kind = (kw.type == TOK_DIE) ? FUNC_DIE : FUNC_TRY;
  f->span = span_join(span_from_token(&kw), body->span);
  f->name = ident_from_token(&name);
  f->params = params;
  f->body = body;

  s = new_stmt(p, STMT_FUNC, f->span);
  s->func_stmt.func = f;
  return s;
}

struct program *parse(const char *src, size_t len, struct arena *arena,
                      struct complain_list *complains) {
  struct parser p;
  struct program *prog;

  parser_init(&p, src, len, arena, complains);
  prog = arena_alloc(arena, sizeof *prog);
  prog->stmts = parse_stmt_list(&p, 0);
  return prog;
}