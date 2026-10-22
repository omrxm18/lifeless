// lifeless - interp.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include "life_support.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

enum value_kind { VAL_NONE, VAL_BOOL, VAL_INT, VAL_STR };
const char *op_text(enum token_type op);

struct value {
  enum value_kind kind; // VAL_NONE is 0: a zeroed value is none
  union {
    int boolean;
    int64_t integer;
    struct {
      const char *data;
      size_t len;
    } str;
  };
};

static struct value v_none(void) {
  struct value v = {.kind = VAL_NONE};
  return v;
}

static struct value v_bool(int b) {
  struct value v = {.kind = VAL_BOOL, .boolean = b != 0};
  return v;
}

static struct value v_int(int64_t n) {
  struct value v = {.kind = VAL_INT, .integer = n};
  return v;
}

static struct value v_str(const char *data, size_t len) {
  struct value v = {.kind = VAL_STR, .str = {data, len}};
  return v;
}

struct rvar {
  struct rvar *next;
  struct ident name;
  struct value value;
  int frozen;
  int defined; // top-level variables start undefined, until their first
               // assignment runs
};

struct rscope {
  struct rscope *parent;
  struct rvar *vars;
};

struct rfunc {
  struct rfunc *next;
  struct ident name;
  const struct func *def; // NULL for the built-in print
};

#define MAX_CALL_DEPTH 1000

struct interp {
  struct arena *arena;
  FILE *out;
  const char *path;

  struct rfunc *funcs;  // every top-level function, plus print
  struct rscope global; // top-level variables
  struct rscope *scope; // innermost scope being run
  int depth;            // function calls in progress, 0 at top level
  struct value ret;     // the value of the last `return`

  jmp_buf on_error;     // where rt_error() jumps to
  struct span err_span; // filled in just before the jump
  char err_msg[256];
};

#if defined(__GNUC__)
#define RT_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define RT_PRINTF(fmt, first)
#endif

// Record the error and abandon the program. Does not return
static _Noreturn void rt_error(struct interp *ip, struct span at,
                               const char *fmt, ...) RT_PRINTF(3, 4);

static _Noreturn void rt_error(struct interp *ip, struct span at,
                               const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(ip->err_msg, sizeof ip->err_msg, fmt, ap);
  va_end(ap);
  ip->err_span = at;
  longjmp(ip->on_error, 1);
}

// printf arguments for an identifier printed with "%.*s"
#define NAME(id) (int)(id).len, (id).text

static int ident_eq(struct ident a, struct ident b) {
  return a.len == b.len && memcmp(a.text, b.text, a.len) == 0;
}

static struct rfunc *find_func(const struct interp *ip, struct ident name) {
  for (struct rfunc *f = ip->funcs; f; f = f->next)
    if (ident_eq(f->name, name))
      return f;
  return NULL;
}

/* A variable in exactly this scope. Variables not yet assigned are
 * invisible. */
static struct rvar *scope_find(const struct rscope *sc, struct ident name) {
  for (struct rvar *v = sc->vars; v; v = v->next)
    if (v->defined && ident_eq(v->name, name))
      return v;
  return NULL;
}

/* Find a variable visible from the current scope. *is_global is set when
 * it is a top-level variable. Inside a function the scope chain ends at
 * the function's own scope; after that every top-level variable counts,
 * defined yet or not (the caller looks at `defined`). */
static struct rvar *lookup(const struct interp *ip, struct ident name,
                           int *is_global) {
  for (const struct rscope *sc = ip->scope; sc; sc = sc->parent) {
    struct rvar *v = scope_find(sc, name);

    if (v) {
      *is_global = (sc == &ip->global);
      return v;
    }
  }
  if (ip->depth > 0) {
    for (struct rvar *v = ip->global.vars; v; v = v->next) {
      if (ident_eq(v->name, name)) {
        *is_global = 1;
        return v;
      }
    }
  }
  *is_global = 0;
  return NULL;
}

/* Create a variable in the current scope. At top level, reuse the entry
 * registered before the run started. The new variable is defined and
 * unfrozen, with value none until the caller sets it. */
static struct rvar *declare(struct interp *ip, struct ident name) {
  struct rvar *v = NULL;

  if (ip->scope == &ip->global) {
    for (v = ip->global.vars; v; v = v->next)
      if (ident_eq(v->name, name))
        break;
  }
  if (!v) {
    v = arena_alloc(ip->arena, sizeof *v);
    v->name = name;
    v->next = ip->scope->vars;
    ip->scope->vars = v;
  }
  v->defined = 1;
  return v;
}

static const char *kind_name(enum value_kind k) {
  switch (k) {
  case VAL_NONE:
    return "none";
  case VAL_BOOL:
    return "boolean";
  case VAL_INT:
    return "integer";
  case VAL_STR:
    return "string";
  }
  return "?";
}

static int value_equal(struct value a, struct value b) {
  if (a.kind != b.kind)
    return 0;
  switch (a.kind) {
  case VAL_NONE:
    return 1;
  case VAL_BOOL:
    return a.boolean == b.boolean;
  case VAL_INT:
    return a.integer == b.integer;
  case VAL_STR:
    return a.str.len == b.str.len &&
           memcmp(a.str.data, b.str.data, a.str.len) == 0;
  }
  return 0;
}

static void require_bool(struct interp *ip, struct value v, struct span at,
                         enum token_type op) {
  if (v.kind != VAL_BOOL)
    rt_error(ip, at, "operand of '%s' must be a boolean, found %s", op_text(op),
             kind_name(v.kind));
}

static struct value concat(struct interp *ip, struct span at, struct value a,
                           struct value b) {
  size_t len;
  char *buf;

  if (a.str.len > SIZE_MAX - b.str.len - 1)
    rt_error(ip, at, "string is too long");
  len = a.str.len + b.str.len;
  buf = arena_alloc(ip->arena, len + 1);
  memcpy(buf, a.str.data, a.str.len);
  memcpy(buf + a.str.len, b.str.data, b.str.len);
  buf[len] = '\0';
  return v_str(buf, len);
}

static int add_overflows(int64_t a, int64_t b) {
  return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

static int sub_overflows(int64_t a, int64_t b) {
  return (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
}

static int mul_overflows(int64_t a, int64_t b) {
  if (a == 0 || b == 0)
    return 0;
  if (a == -1)
    return b == INT64_MIN;
  if (b == -1)
    return a == INT64_MIN;
  if (a > 0)
    return b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
  return b > 0 ? a < INT64_MIN / b : b < INT64_MAX / a;
}

static struct value eval_expr(struct interp *ip, const struct expr *e);
static struct value eval_call(struct interp *ip, const struct expr *e);

static void require_ints(struct interp *ip, const struct expr *e,
                         struct value a, struct value b) {
  if (a.kind != VAL_INT || b.kind != VAL_INT)
    rt_error(ip, e->span, "'%s' needs integers, found %s and %s",
             op_text(e->binary.op), kind_name(a.kind), kind_name(b.kind));
}

static struct value eval_unary(struct interp *ip, const struct expr *e) {
  struct value v = eval_expr(ip, e->unary.operand);

  if (e->unary.op == TOK_NOT) {
    if (v.kind != VAL_BOOL)
      rt_error(ip, e->span, "'!' needs a boolean, found %s", kind_name(v.kind));
    return v_bool(!v.boolean);
  }

  if (v.kind != VAL_INT)
    rt_error(ip, e->span, "unary '-' needs an integer, found %s",
             kind_name(v.kind));
  if (v.integer == INT64_MIN)
    rt_error(ip, e->span, "integer overflow");
  return v_int(-v.integer);
}

static struct value eval_binary(struct interp *ip, const struct expr *e) {
  enum token_type op = e->binary.op;
  struct value a = eval_expr(ip, e->binary.lhs);
  struct value b;

  // && and || short-circuit: the right side runs only if it is needed
  if (op == TOK_AND || op == TOK_OR) {
    require_bool(ip, a, e->binary.lhs->span, op);
    if (op == TOK_AND ? !a.boolean : a.boolean)
      return a;
    b = eval_expr(ip, e->binary.rhs);
    require_bool(ip, b, e->binary.rhs->span, op);
    return b;
  }

  b = eval_expr(ip, e->binary.rhs);

  switch (op) {
  case TOK_EQ:
    return v_bool(value_equal(a, b));
  case TOK_NE:
    return v_bool(!value_equal(a, b));

  case TOK_PLUS:
    if (a.kind == VAL_STR && b.kind == VAL_STR)
      return concat(ip, e->span, a, b);
    if (a.kind != VAL_INT || b.kind != VAL_INT)
      rt_error(ip, e->span,
               "'+' needs two integers or two strings, found %s and %s",
               kind_name(a.kind), kind_name(b.kind));
    if (add_overflows(a.integer, b.integer))
      rt_error(ip, e->span, "integer overflow");
    return v_int(a.integer + b.integer);

  case TOK_MINUS:
    require_ints(ip, e, a, b);
    if (sub_overflows(a.integer, b.integer))
      rt_error(ip, e->span, "integer overflow");
    return v_int(a.integer - b.integer);

  case TOK_STAR:
    require_ints(ip, e, a, b);
    if (mul_overflows(a.integer, b.integer))
      rt_error(ip, e->span, "integer overflow");
    return v_int(a.integer * b.integer);

  case TOK_SLASH:
    require_ints(ip, e, a, b);
    if (b.integer == 0)
      rt_error(ip, e->span, "division by zero");
    if (a.integer == INT64_MIN && b.integer == -1)
      rt_error(ip, e->span, "integer overflow");
    return v_int(a.integer / b.integer);

  case TOK_PERCENT:
    require_ints(ip, e, a, b);
    if (b.integer == 0)
      rt_error(ip, e->span, "division by zero");
    if (b.integer == -1)
      return v_int(0); // avoids INT64_MIN % -1
    return v_int(a.integer % b.integer);

  case TOK_LT:
    require_ints(ip, e, a, b);
    return v_bool(a.integer < b.integer);
  case TOK_LE:
    require_ints(ip, e, a, b);
    return v_bool(a.integer <= b.integer);
  case TOK_GT:
    require_ints(ip, e, a, b);
    return v_bool(a.integer > b.integer);
  case TOK_GE:
    require_ints(ip, e, a, b);
    return v_bool(a.integer >= b.integer);

  default:
    break;
  }
  rt_error(ip, e->span, "internal error: unknown operator");
}

static struct value eval_expr(struct interp *ip, const struct expr *e) {
  switch (e->kind) {
  case EXPR_INT:
    return v_int(e->int_value);
  case EXPR_STR:
    return v_str(e->str.data, e->str.len);
  case EXPR_BOOL:
    return v_bool(e->bool_value);
  case EXPR_NONE:
    return v_none();
  case EXPR_VAR: {
    int is_global;
    struct rvar *v = lookup(ip, e->var, &is_global);

    if (!v)
      rt_error(ip, e->var.span, "undefined variable '%.*s'", NAME(e->var));
    if (!v->defined)
      rt_error(ip, e->var.span, "variable '%.*s' is read before it is assigned",
               NAME(e->var));
    return v->value;
  }
  case EXPR_CALL:
    return eval_call(ip, e);
  case EXPR_UNARY:
    return eval_unary(ip, e);
  case EXPR_BINARY:
    return eval_binary(ip, e);
  }
  rt_error(ip, e->span, "internal error: unknown expression");
}

/* What running a statement did: carry on, or a `return` ran (its value is
 * in ip->ret and the enclosing function should stop). */
enum exec_result { EXEC_NEXT, EXEC_RETURN };

static enum exec_result exec_stmt(struct interp *ip, const struct stmt *s);
static enum exec_result exec_stmts(struct interp *ip, const struct stmt *s);

// An if or while condition must be a boolean
static int eval_condition(struct interp *ip, const struct expr *cond) {
  struct value v = eval_expr(ip, cond);

  if (v.kind != VAL_BOOL)
    rt_error(ip, cond->span, "condition must be a boolean, found %s",
             kind_name(v.kind));
  return v.boolean;
}

// Run a block in a scope of its own
static enum exec_result exec_block(struct interp *ip, const struct block *b) {
  struct rscope sc = {.parent = ip->scope};
  enum exec_result r;

  ip->scope = &sc;
  r = exec_stmts(ip, b->stmts);
  ip->scope = sc.parent;
  return r;
}

/* `name = value`: change the variable if one is visible, otherwise create
 * one in the current scope. */
static void assign_var(struct interp *ip, struct ident name,
                       struct value value) {
  int is_global;
  struct rvar *v = lookup(ip, name, &is_global);

  if (!v)
    v = declare(ip, name);
  else if (v->frozen)
    rt_error(ip, name.span, "'%.*s' is frozen and can't be assigned to",
             NAME(name));
  v->value = value;
  v->defined = 1;
}

static enum exec_result exec_stmt(struct interp *ip, const struct stmt *s) {
  switch (s->kind) {
  case STMT_EXPR:
    eval_expr(ip, s->expr_stmt.expr);
    return EXEC_NEXT;

  case STMT_ASSIGN:
    assign_var(ip, s->assign_stmt.target, eval_expr(ip, s->assign_stmt.value));
    return EXEC_NEXT;

  case STMT_RO: {
    struct ident name = s->ro_stmt.target;
    struct rvar *v;

    if (s->ro_stmt.value)
      assign_var(ip, name, eval_expr(ip, s->ro_stmt.value));
    v = scope_find(ip->scope, name);
    if (!v)
      rt_error(ip, name.span,
               "can't freeze '%.*s': no such variable in the current scope",
               NAME(name));
    v->frozen = 1;
    return EXEC_NEXT;
  }

  case STMT_RETURN:
    ip->ret =
        s->return_stmt.value ? eval_expr(ip, s->return_stmt.value) : v_none();
    return EXEC_RETURN;

  case STMT_IF:
    if (eval_condition(ip, s->if_stmt.cond))
      return exec_block(ip, s->if_stmt.then_block);
    if (s->if_stmt.else_if)
      return exec_stmt(ip, s->if_stmt.else_if);
    if (s->if_stmt.else_block)
      return exec_block(ip, s->if_stmt.else_block);
    return EXEC_NEXT;

  case STMT_WHILE:
    while (eval_condition(ip, s->while_stmt.cond))
      if (exec_block(ip, s->while_stmt.body) == EXEC_RETURN)
        return EXEC_RETURN;
    return EXEC_NEXT;

  case STMT_FUNC:
    return EXEC_NEXT; // functions are registered before the run starts
  }
  return EXEC_NEXT;
}

static enum exec_result exec_stmts(struct interp *ip, const struct stmt *s) {
  for (; s; s = s->next)
    if (exec_stmt(ip, s) == EXEC_RETURN)
      return EXEC_RETURN;
  return EXEC_NEXT;
}

static void print_value(const struct interp *ip, struct value v) {
  switch (v.kind) {
  case VAL_NONE:
    fputs("none", ip->out);
    break;
  case VAL_BOOL:
    fputs(v.boolean ? "true" : "false", ip->out);
    break;
  case VAL_INT:
    fprintf(ip->out, "%lld", (long long)v.integer);
    break;
  case VAL_STR:
    fwrite(v.str.data, 1, v.str.len, ip->out);
    break;
  }
  fputc('\n', ip->out);
}

static int count_args(const struct expr *a) {
  int n = 0;

  for (; a; a = a->next)
    n++;
  return n;
}

static int count_params(const struct param *p) {
  int n = 0;

  for (; p; p = p->next)
    n++;
  return n;
}

static struct value eval_call(struct interp *ip, const struct expr *e) {
  struct ident callee = e->call.callee;
  struct rfunc *f = find_func(ip, callee);
  int nargs = count_args(e->call.args);
  int nparams;
  const struct func *def;
  struct rscope fscope = {.parent = NULL};
  struct rvar *tail = NULL;
  const struct param *pm;
  struct rscope *saved_scope;
  enum exec_result r;
  struct value result;

  if (!f)
    rt_error(ip, callee.span, "undefined function '%.*s'", NAME(callee));

  if (!f->def) {
    struct value v;

    if (nargs != 1)
      rt_error(ip, callee.span, "'print' expects 1 argument, found %d", nargs);
    v = eval_expr(ip, e->call.args);
    print_value(ip, v);
    return v_none();
  }

  def = f->def;
  nparams = count_params(def->params);
  if (nargs != nparams)
    rt_error(ip, callee.span, "'%.*s' expects %d argument%s, found %d",
             NAME(callee), nparams, nparams == 1 ? "" : "s", nargs);
  if (ip->depth >= MAX_CALL_DEPTH)
    rt_error(ip, e->span, "call stack too deep (limit %d)", MAX_CALL_DEPTH);

  /* Evaluate the arguments in the caller's scope, binding each to its
   * parameter in the function's new scope. */
  pm = def->params;
  for (const struct expr *a = e->call.args; a; a = a->next, pm = pm->next) {
    struct value v = eval_expr(ip, a);
    struct rvar *var = arena_alloc(ip->arena, sizeof *var);

    var->name = pm->name;
    var->value = v;
    var->frozen = pm->is_ro;
    var->defined = 1;
    if (tail)
      tail->next = var;
    else
      fscope.vars = var;
    tail = var;
  }

  // Run the body in the new scope
  saved_scope = ip->scope;
  ip->scope = &fscope;
  ip->depth++;
  r = exec_stmts(ip, def->body->stmts);
  ip->depth--;
  ip->scope = saved_scope;

  result = (r == EXEC_RETURN) ? ip->ret : v_none();
  return result;
}

/* Register print and every top-level function before running, so a call
 * may come before the declaration. */
static void register_funcs(struct interp *ip, const struct program *p) {
  struct ident print_name = {.text = "print", .len = 5};
  struct rfunc *f = arena_alloc(ip->arena, sizeof *f);

  f->name = print_name;
  f->def = NULL;
  f->next = ip->funcs;
  ip->funcs = f;

  for (const struct stmt *s = p->stmts; s; s = s->next) {
    if (s->kind != STMT_FUNC)
      continue;
    f = arena_alloc(ip->arena, sizeof *f);
    f->name = s->func_stmt.func->name;
    f->def = s->func_stmt.func;
    f->next = ip->funcs;
    ip->funcs = f;
  }
}

/* Register every variable assigned directly at top level as a global that
 * has not been assigned yet (defined stays 0), the same pre-pass the
 * checker makes. */
static void register_globals(struct interp *ip, const struct program *p) {
  for (const struct stmt *s = p->stmts; s; s = s->next) {
    struct ident name;
    struct rvar *v;

    if (s->kind == STMT_ASSIGN)
      name = s->assign_stmt.target;
    else if (s->kind == STMT_RO && s->ro_stmt.value)
      name = s->ro_stmt.target;
    else
      continue;

    for (v = ip->global.vars; v; v = v->next)
      if (ident_eq(v->name, name))
        break;
    if (v)
      continue;

    v = arena_alloc(ip->arena, sizeof *v);
    v->name = name;
    v->next = ip->global.vars;
    ip->global.vars = v;
  }
}

int run(const struct program *p, struct arena *arena, const char *path,
        FILE *out) {
  /* The state lives in the arena, not on this function's stack. That
   * matters for setjmp: after a longjmp, ordinary local variables that
   * changed since the setjmp have indeterminate values, but memory
   * reached through an unchanged pointer is fine. */
  struct interp *ip = arena_alloc(arena, sizeof *ip);

  ip->arena = arena;
  ip->out = out;
  ip->path = path;
  ip->scope = &ip->global;

  register_funcs(ip, p);
  register_globals(ip, p);

  if (setjmp(ip->on_error)) {
    fflush(out); // so output printed before the error comes first
    fprintf(stderr, "%s:%d:%d: runtime error: %s\n", path, ip->err_span.line,
            ip->err_span.column, ip->err_msg);
    return 1;
  }

  exec_stmts(ip, p->stmts);
  return 0;
}