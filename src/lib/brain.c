// lifeless - ast.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include "brain.h"

struct span span_from_token(const struct token *t) {
  struct span s;

  s.offset = t->offset;
  s.length = t->length;
  s.line = t->line;
  s.column = t->column;
  return s;
}

struct span span_join(struct span a, struct span b) {
  struct span s = a;

  s.length = (b.offset + b.length) - a.offset;
  return s;
}

const char *op_text(enum token_type op) {
  switch (op) {
  case TOK_PLUS:
    return "+";
  case TOK_MINUS:
    return "-";
  case TOK_STAR:
    return "*";
  case TOK_SLASH:
    return "/";
  case TOK_PERCENT:
    return "%";
  case TOK_EQ:
    return "==";
  case TOK_NE:
    return "!=";
  case TOK_LT:
    return "<";
  case TOK_LE:
    return "<=";
  case TOK_GT:
    return ">";
  case TOK_GE:
    return ">=";
  case TOK_AND:
    return "&&";
  case TOK_OR:
    return "||";
  case TOK_NOT:
    return "!";
  default:
    return "?";
  }
}

static void print_quoted(FILE *out, const char *s, size_t n) {
  fputc('"', out);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];

    if (c == '"' || c == '\\')
      fprintf(out, "\\%c", c);
    else if (c == '\n')
      fputs("\\n", out);
    else if (c == '\t')
      fputs("\\t", out);
    else if (c < 0x20 || c == 0x7f)
      fprintf(out, "\\x%02x", c);
    else
      fputc(c, out);
  }
  fputc('"', out);
}

static void print_indent(FILE *out, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", out);
}

static void print_expr(FILE *out, const struct expr *e) {
  switch (e->kind) {
  case EXPR_INT:
    fprintf(out, "%lld", (long long)e->int_value);
    break;
  case EXPR_STR:
    print_quoted(out, e->str.data, e->str.len);
    break;
  case EXPR_BOOL:
    fputs(e->bool_value ? "true" : "false", out);
    break;
  case EXPR_NONE:
    fputs("none", out);
    break;
  case EXPR_VAR:
    fprintf(out, "%.*s", (int)e->var.len, e->var.text);
    break;
  case EXPR_CALL:
    fprintf(out, "(call %.*s", (int)e->call.callee.len, e->call.callee.text);
    for (const struct expr *a = e->call.args; a; a = a->next) {
      fputc(' ', out);
      print_expr(out, a);
    }
    fputc(')', out);
    break;
  case EXPR_UNARY:
    fprintf(out, "(%s ", op_text(e->unary.op));
    print_expr(out, e->unary.operand);
    fputc(')', out);
    break;
  case EXPR_BINARY:
    fprintf(out, "(%s ", op_text(e->binary.op));
    print_expr(out, e->binary.lhs);
    fputc(' ', out);
    print_expr(out, e->binary.rhs);
    fputc(')', out);
    break;
  }
}

static void print_stmts(FILE *out, const struct stmt *s, int depth);

// `lead` is "if" for the first branch and "else if" for the ones after it
static void print_if(FILE *out, const struct stmt *s, int depth,
                     const char *lead) {
  print_indent(out, depth);
  fprintf(out, "%s ", lead);
  print_expr(out, s->if_stmt.cond);
  fputc('\n', out);
  print_stmts(out, s->if_stmt.then_block->stmts, depth + 1);

  if (s->if_stmt.else_if) {
    print_if(out, s->if_stmt.else_if, depth, "else if");
  } else if (s->if_stmt.else_block) {
    print_indent(out, depth);
    fputs("else\n", out);
    print_stmts(out, s->if_stmt.else_block->stmts, depth + 1);
  }
}

static void print_stmts(FILE *out, const struct stmt *s, int depth) {
  for (; s; s = s->next) {
    switch (s->kind) {
    case STMT_IF:
      print_if(out, s, depth, "if");
      break;
    case STMT_WHILE:
      print_indent(out, depth);
      fputs("while ", out);
      print_expr(out, s->while_stmt.cond);
      fputc('\n', out);
      print_stmts(out, s->while_stmt.body->stmts, depth + 1);
      break;
    case STMT_RETURN:
      print_indent(out, depth);
      fputs("return", out);
      if (s->return_stmt.value) {
        fputc(' ', out);
        print_expr(out, s->return_stmt.value);
      }
      fputc('\n', out);
      break;
    case STMT_RO:
      print_indent(out, depth);
      fprintf(out, "ro %.*s", (int)s->ro_stmt.target.len,
              s->ro_stmt.target.text);
      if (s->ro_stmt.value) {
        fputs(" = ", out);
        print_expr(out, s->ro_stmt.value);
      }
      fputc('\n', out);
      break;
    case STMT_ASSIGN:
      print_indent(out, depth);
      fprintf(out, "%.*s = ", (int)s->assign_stmt.target.len,
              s->assign_stmt.target.text);
      print_expr(out, s->assign_stmt.value);
      fputc('\n', out);
      break;
    case STMT_EXPR:
      print_indent(out, depth);
      fputs("expr ", out);
      print_expr(out, s->expr_stmt.expr);
      fputc('\n', out);
      break;
    case STMT_FUNC: {
      const struct func *f = s->func_stmt.func;

      print_indent(out, depth);
      fprintf(out, "func %s %.*s(", f->kind == FUNC_DIE ? "die" : "try",
              (int)f->name.len, f->name.text);
      for (const struct param *p = f->params; p; p = p->next) {
        if (p != f->params)
          fputs(", ", out);
        if (p->is_ro)
          fputs("ro ", out);
        fprintf(out, "%.*s", (int)p->name.len, p->name.text);
      }
      fputs(")\n", out);
      print_stmts(out, f->body->stmts, depth + 1);
      break;
    }
    }
  }
}

void brain_print(const struct program *p, FILE *out) {
  print_stmts(out, p->stmts, 0);
}