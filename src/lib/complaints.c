// lifeless - complaints.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdarg.h>
#include <string.h>

#include "arena.h"
#include "brain.h"
#include "complaints.h"

struct var {
  struct var *next;
  struct ident name;
  int frozen;  // ro has been applied
  int defined; /* the walk has reached its first assignment (top-level
                * variables start at 0, everything else at 1) */
};

struct scope {
  struct scope *parent;
  struct var *vars;
};

// A function known to the checker: a top-level declaration, or print.
struct fninfo {
  struct fninfo *next;
  struct ident name;
  enum func_kind kind;
  int nparams;
  int builtin;
  struct span span; // where it was declared (unused for built-ins)
};

struct checker {
  struct arena *arena; // scratch memory: the diagnostics' own arena
  struct complain_list *complains;
  struct fninfo *funcs;    // every top-level function, plus print
  struct scope global;     // variables assigned at top level
  struct scope *scope;     // innermost scope being checked
  const struct func *func; // function being checked, NULL at top level
};

// printf arguments for an identifier printed with "%.*s"
#define NAME(id) (int)(id).len, (id).text

/* Record an error at a span. `sp` is evaluated several times, so pass a
 * plain expression such as `name.span`. */
#define error_at(ck, sp, ...)                                                  \
  complain_add((ck)->complains, (sp).line, (sp).column, (sp).offset,           \
               (sp).length, __VA_ARGS__)

static int ident_eq(struct ident a, struct ident b) {
  return a.len == b.len && memcmp(a.text, b.text, a.len) == 0;
}

static struct fninfo *find_func(const struct checker *ck, struct ident name) {
  for (struct fninfo *f = ck->funcs; f; f = f->next)
    if (ident_eq(f->name, name))
      return f;
  return NULL;
}

static struct fninfo *add_func(struct checker *ck, struct ident name,
                               enum func_kind kind, int nparams, int builtin,
                               struct span span) {
  struct fninfo *f = arena_alloc(ck->arena, sizeof *f);

  f->name = name;
  f->kind = kind;
  f->nparams = nparams;
  f->builtin = builtin;
  f->span = span;
  f->next = ck->funcs;
  ck->funcs = f;
  return f;
}

/* A variable in exactly this scope. Variables the walk has not reached
 * yet are invisible. */
static struct var *scope_find(const struct scope *sc, struct ident name) {
  for (struct var *v = sc->vars; v; v = v->next)
    if (v->defined && ident_eq(v->name, name))
      return v;
  return NULL;
}

/* Find a variable visible from the current scope. *is_global is set when
 * it is a top-level variable. Inside a function the scope chain ends at
 * the function's own scope; after that, every top-level variable counts,
 * whether or not the walk has reached it yet, because the function runs
 * later. */
static struct var *lookup(const struct checker *ck, struct ident name,
                          int *is_global) {
  for (const struct scope *sc = ck->scope; sc; sc = sc->parent) {
    struct var *v = scope_find(sc, name);

    if (v) {
      *is_global = (sc == &ck->global);
      return v;
    }
  }
  if (ck->func) {
    for (struct var *v = ck->global.vars; v; v = v->next) {
      if (ident_eq(v->name, name)) {
        *is_global = 1;
        return v;
      }
    }
  }
  *is_global = 0;
  return NULL;
}

/* Create a variable in the current scope (the caller has already checked
 * that none exists). At top level, reuses the entry the pre-pass made. */
static struct var *declare(struct checker *ck, struct ident name) {
  struct var *v = NULL;

  if (find_func(ck, name))
    error_at(ck, name.span,
             "'%.*s' is the name of a function and can't be a variable",
             NAME(name));

  if (ck->scope == &ck->global) {
    for (v = ck->global.vars; v; v = v->next)
      if (ident_eq(v->name, name))
        break;
  }
  if (!v) {
    v = arena_alloc(ck->arena, sizeof *v);
    v->name = name;
    v->next = ck->scope->vars;
    ck->scope->vars = v;
  }
  v->defined = 1;
  return v;
}

static void check_var_read(struct checker *ck, const struct expr *e) {
  int is_global;
  struct var *v = lookup(ck, e->var, &is_global);

  if (!v) {
    if (find_func(ck, e->var))
      error_at(ck, e->var.span, "'%.*s' is a function; call it as %.*s(...)",
               NAME(e->var), NAME(e->var));
    else
      error_at(ck, e->var.span, "undefined variable '%.*s'", NAME(e->var));
    return;
  }
  if (is_global && ck->func && ck->func->kind == FUNC_DIE && !v->frozen)
    error_at(ck, e->var.span,
             "die function can't read global variable '%.*s' "
             "(freeze it with ro before the function)",
             NAME(e->var));
}

static void check_call(struct checker *ck, const struct expr *e) {
  struct ident callee = e->call.callee;
  const struct fninfo *f = find_func(ck, callee);
  int nargs = 0;

  for (const struct expr *a = e->call.args; a; a = a->next)
    nargs++;

  if (!f) {
    error_at(ck, callee.span, "undefined function '%.*s'", NAME(callee));
    return;
  }
  if (nargs != f->nparams)
    error_at(ck, callee.span, "'%.*s' expects %d argument%s, found %d",
             NAME(callee), f->nparams, f->nparams == 1 ? "" : "s", nargs);
  if (ck->func && ck->func->kind == FUNC_DIE && f->kind == FUNC_TRY)
    error_at(ck, callee.span, "die function can't call try function '%.*s'",
             NAME(callee));
}

static void check_expr(struct checker *ck, const struct expr *e) {
  switch (e->kind) {
  case EXPR_INT:
  case EXPR_STR:
  case EXPR_BOOL:
  case EXPR_NONE:
    break;
  case EXPR_VAR:
    check_var_read(ck, e);
    break;
  case EXPR_CALL:
    for (const struct expr *a = e->call.args; a; a = a->next)
      check_expr(ck, a);
    check_call(ck, e);
    break;
  case EXPR_UNARY:
    check_expr(ck, e->unary.operand);
    break;
  case EXPR_BINARY:
    check_expr(ck, e->binary.lhs);
    check_expr(ck, e->binary.rhs);
    break;
  }
}

/* Assign to `name`: a new variable if it does not exist yet, otherwise
 * a change to the existing one. The right-hand side is checked first by
 * the caller, so `x = x + 1` with no x yet is an undefined read. */
static void check_assign(struct checker *ck, struct ident name) {
  int is_global;
  struct var *v = lookup(ck, name, &is_global);

  if (!v) { /* new, local to this scope */
    declare(ck, name);
    return;
  }
  if (is_global && ck->func && ck->func->kind == FUNC_DIE) {
    error_at(ck, name.span, "die function can't change global variable '%.*s'",
             NAME(name));
    return;
  }
  if (v->frozen)
    error_at(ck, name.span, "'%.*s' is frozen and can't be assigned to",
             NAME(name));
}

// `ro x;` and `ro x = value;`
static void check_ro(struct checker *ck, const struct stmt *s) {
  struct ident name = s->ro_stmt.target;
  int is_global;
  struct var *v;

  if (s->ro_stmt.value)
    check_expr(ck, s->ro_stmt.value);

  v = scope_find(ck->scope, name); // the current scope only
  if (v) {
    if (s->ro_stmt.value && v->frozen)
      error_at(ck, name.span, "'%.*s' is frozen and can't be assigned to",
               NAME(name));
    v->frozen = 1;
    return;
  }

  if (lookup(ck, name, &is_global)) { // it exists, but in an outer scope
    error_at(ck, name.span,
             "can't freeze '%.*s': ro only works on variables created "
             "in the current scope",
             NAME(name));
    return;
  }

  if (s->ro_stmt.value)
    declare(ck, name)->frozen = 1;
  else
    error_at(ck, name.span, "can't freeze '%.*s': it is not defined",
             NAME(name));
}

static void check_func(struct checker *ck, const struct func *f);
static void check_stmts(struct checker *ck, const struct stmt *s);

static void check_block(struct checker *ck, const struct block *b) {
  struct scope sc = {.parent = ck->scope};

  ck->scope = &sc;
  check_stmts(ck, b->stmts);
  ck->scope = sc.parent;
}

static void check_stmt(struct checker *ck, const struct stmt *s) {
  switch (s->kind) {
  case STMT_EXPR:
    check_expr(ck, s->expr_stmt.expr);
    break;
  case STMT_RETURN:
    if (!ck->func)
      error_at(ck, s->span, "'return' outside of a function");
    if (s->return_stmt.value)
      check_expr(ck, s->return_stmt.value);
    break;
  case STMT_ASSIGN:
    check_expr(ck, s->assign_stmt.value);
    check_assign(ck, s->assign_stmt.target);
    break;
  case STMT_RO:
    check_ro(ck, s);
    break;
  case STMT_IF:
    check_expr(ck, s->if_stmt.cond);
    check_block(ck, s->if_stmt.then_block);
    if (s->if_stmt.else_if)
      check_stmt(ck, s->if_stmt.else_if);
    else if (s->if_stmt.else_block)
      check_block(ck, s->if_stmt.else_block);
    break;
  case STMT_WHILE:
    check_expr(ck, s->while_stmt.cond);
    check_block(ck, s->while_stmt.body);
    break;
  case STMT_FUNC:
    /* Top-level functions are checked right here, in file order. One
     * inside a block was already reported by the parser: skip it. */
    if (!ck->func && ck->scope == &ck->global)
      check_func(ck, s->func_stmt.func);
    break;
  }
}

static void check_stmts(struct checker *ck, const struct stmt *s) {
  for (; s; s = s->next)
    check_stmt(ck, s);
}

static void check_func(struct checker *ck, const struct func *f) {
  struct scope fscope = {.parent = NULL};
  struct scope *saved_scope = ck->scope;
  const struct func *saved_func = ck->func;

  ck->scope = &fscope;
  ck->func = f;

  /* Parameters and the body's top-level statements share one scope, so
   * `ro n;` at the start of the body works on a parameter. */
  for (const struct param *p = f->params; p; p = p->next) {
    if (scope_find(&fscope, p->name))
      error_at(ck, p->name.span, "duplicate parameter '%.*s'", NAME(p->name));
    else
      declare(ck, p->name)->frozen = p->is_ro;
  }

  check_stmts(ck, f->body->stmts);

  ck->scope = saved_scope;
  ck->func = saved_func;
}

/* Register every top-level function before the walk, so a call may come
 * before the declaration. Reports duplicates and attempts to redefine print. */
static void collect_funcs(struct checker *ck, const struct program *p) {
  for (const struct stmt *s = p->stmts; s; s = s->next) {
    const struct func *f;
    const struct fninfo *prev;
    int n = 0;

    if (s->kind != STMT_FUNC)
      continue;
    f = s->func_stmt.func;
    for (const struct param *pm = f->params; pm; pm = pm->next)
      n++;

    prev = find_func(ck, f->name);
    if (prev && prev->builtin)
      error_at(ck, f->name.span,
               "'%.*s' is a built-in function and can't be redefined",
               NAME(f->name));
    else if (prev)
      error_at(ck, f->name.span, "function '%.*s' is already defined at %d:%d",
               NAME(f->name), prev->span.line, prev->span.column);
    else
      add_func(ck, f->name, f->kind, n, 0, f->name.span);
  }
}

/* Register every variable assigned directly at top level (not inside a
 * block) as a global the walk has not reached yet (defined stays 0). */
static void collect_globals(struct checker *ck, const struct program *p) {
  for (const struct stmt *s = p->stmts; s; s = s->next) {
    struct ident name;
    struct var *v;

    if (s->kind == STMT_ASSIGN)
      name = s->assign_stmt.target;
    else if (s->kind == STMT_RO && s->ro_stmt.value)
      name = s->ro_stmt.target;
    else
      continue;

    for (v = ck->global.vars; v; v = v->next)
      if (ident_eq(v->name, name))
        break;
    if (v)
      continue;

    v = arena_alloc(ck->arena, sizeof *v);
    v->name = name;
    v->next = ck->global.vars;
    ck->global.vars = v;
  }
}

void check(const struct program *p, struct complain_list *complains) {
  struct checker ck = {.arena = complains->arena, .complains = complains};
  struct ident print_name = {.text = "print", .len = 5};

  ck.scope = &ck.global;
  add_func(&ck, print_name, FUNC_TRY, 1, 1, print_name.span);

  collect_funcs(&ck, p);
  collect_globals(&ck, p);
  check_stmts(&ck, p->stmts);
}

void complain_init(struct complain_list *d, struct arena *arena) {
  d->arena = arena;
  d->head = NULL;
  d->tail = NULL;
  d->count = 0;
}

void complain_add(struct complain_list *d, int line, int column, size_t offset,
                  size_t length, const char *fmt, ...) {
  va_list ap, ap2;
  const char *msg;
  struct complaints *e;
  int n;

  // Format twice: once to measure, once to write into exact-size memory
  va_start(ap, fmt);
  va_copy(ap2, ap);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (n < 0) {
    msg = "invalid diagnostic";
  } else {
    char *buf = arena_alloc(d->arena, (size_t)n + 1);

    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    msg = buf;
  }
  va_end(ap2);

  e = arena_alloc(d->arena, sizeof *e);
  e->line = line;
  e->column = column;
  e->offset = offset;
  e->length = length;
  e->message = msg;

  if (d->tail)
    d->tail->next = e;
  else
    d->head = e;
  d->tail = e;
  d->count++;
}

void complain_print(const struct complain_list *d, const char *path,
                    FILE *out) {
  for (const struct complaints *e = d->head; e; e = e->next)
    fprintf(out, "%s:%d:%d: error: %s\n", path, e->line, e->column, e->message);
}