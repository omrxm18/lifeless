/* lifeful - server.c
 * Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "complaints.h"
#include "json.h"
#include "parser.h"
#include "version.h"

enum {
  MAX_HEADER_LINE = 1024,
  MAX_HEADER_BYTES = 8192,
  MAX_MESSAGE_SIZE = 16 * 1024 * 1024
};

struct strbuf {
  char *data;
  size_t len;
  size_t cap;
};

struct document {
  struct document *next;
  char *uri;
  char *text;
  size_t text_len;
  long version;
  int has_version;
};

struct server {
  FILE *input;
  FILE *output;
  FILE *errors;
  struct document *documents;
  int shutdown_requested;
};

static int sb_reserve(struct strbuf *b, size_t extra) {
  size_t needed;
  size_t cap;
  char *data;

  if (extra > SIZE_MAX - b->len - 1)
    return 0;
  needed = b->len + extra + 1;
  if (needed <= b->cap)
    return 1;
  cap = b->cap ? b->cap : 256;
  while (cap < needed) {
    if (cap > SIZE_MAX / 2) {
      cap = needed;
      break;
    }
    cap *= 2;
  }
  data = realloc(b->data, cap);
  if (!data)
    return 0;
  b->data = data;
  b->cap = cap;
  return 1;
}

static int sb_append_n(struct strbuf *b, const char *s, size_t n) {
  if (!sb_reserve(b, n))
    return 0;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 1;
}

static int sb_append(struct strbuf *b, const char *s) {
  return sb_append_n(b, s, strlen(s));
}

static int sb_printf(struct strbuf *b, const char *fmt, ...) {
  va_list ap;
  va_list copy;
  int n;

  va_start(ap, fmt);
  va_copy(copy, ap);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0 || !sb_reserve(b, (size_t)n)) {
    va_end(copy);
    return 0;
  }
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, copy);
  va_end(copy);
  b->len += (size_t)n;
  return 1;
}

static int sb_json_string(struct strbuf *b, const char *s) {
  static const char hex[] = "0123456789abcdef";
  if (!sb_append(b, "\""))
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    char escaped[6];
    switch (*p) {
    case '"':
      if (!sb_append(b, "\\\""))
        return 0;
      break;
    case '\\':
      if (!sb_append(b, "\\\\"))
        return 0;
      break;
    case '\b':
      if (!sb_append(b, "\\b"))
        return 0;
      break;
    case '\f':
      if (!sb_append(b, "\\f"))
        return 0;
      break;
    case '\n':
      if (!sb_append(b, "\\n"))
        return 0;
      break;
    case '\r':
      if (!sb_append(b, "\\r"))
        return 0;
      break;
    case '\t':
      if (!sb_append(b, "\\t"))
        return 0;
      break;
    default:
      if (*p < 0x20) {
        escaped[0] = '\\';
        escaped[1] = 'u';
        escaped[2] = '0';
        escaped[3] = '0';
        escaped[4] = hex[*p >> 4];
        escaped[5] = hex[*p & 0x0f];
        if (!sb_append_n(b, escaped, sizeof escaped))
          return 0;
      } else if (!sb_append_n(b, (const char *)p, 1)) {
        return 0;
      }
      break;
    }
  }
  return sb_append(b, "\"");
}

static void sb_destroy(struct strbuf *b) {
  free(b->data);
  memset(b, 0, sizeof *b);
}

static int write_message(struct server *s, const char *body, size_t len) {
  if (fprintf(s->output, "Content-Length: %zu\r\n\r\n", len) < 0 ||
      fwrite(body, 1, len, s->output) != len || fflush(s->output) == EOF)
    return 0;
  return 1;
}

static int send_buffer(struct server *s, struct strbuf *b) {
  int ok = write_message(s, b->data ? b->data : "", b->len);
  sb_destroy(b);
  return ok;
}

static int send_response(struct server *s, const char *id, size_t id_len,
                         const char *result) {
  struct strbuf b = {0};
  int ok = sb_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":") &&
           sb_append_n(&b, id, id_len) && sb_append(&b, ",\"result\":") &&
           sb_append(&b, result) && sb_append(&b, "}");
  if (!ok) {
    sb_destroy(&b);
    return 0;
  }
  return send_buffer(s, &b);
}

static int send_error(struct server *s, const char *id, size_t id_len, int code,
                      const char *message) {
  struct strbuf b = {0};
  int ok = sb_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":") &&
           sb_append_n(&b, id, id_len) &&
           sb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code) &&
           sb_json_string(&b, message) && sb_append(&b, "}}");
  if (!ok) {
    sb_destroy(&b);
    return 0;
  }
  return send_buffer(s, &b);
}

static int read_line(FILE *in, char *buf, size_t cap) {
  size_t len = 0;
  int c;

  while ((c = fgetc(in)) != EOF && c != '\n') {
    if (len + 1 >= cap) {
      while (c != EOF && c != '\n')
        c = fgetc(in);
      return -1;
    }
    buf[len++] = (char)c;
  }
  if (c == EOF && len == 0)
    return 0;
  if (c == EOF)
    return -1;
  if (len && buf[len - 1] == '\r')
    len--;
  buf[len] = '\0';
  return 1;
}

static int header_name_is(const char *line, const char *name) {
  size_t n = strlen(name);
  if (strlen(line) <= n || line[n] != ':')
    return 0;
  for (size_t i = 0; i < n; i++) {
    if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i]))
      return 0;
  }
  return 1;
}

static int read_message(FILE *in, char **out, size_t *out_len) {
  char line[MAX_HEADER_LINE];
  size_t header_bytes = 0;
  size_t content_length = 0;
  int have_length = 0;

  *out = NULL;
  *out_len = 0;
  for (;;) {
    int status = read_line(in, line, sizeof line);
    if (status <= 0) {
      if (status == 0 && header_bytes == 0)
        return 0;
      return -1;
    }
    header_bytes += strlen(line) + 2;
    if (header_bytes > MAX_HEADER_BYTES)
      return -1;
    if (!line[0])
      break;

    if (header_name_is(line, "Content-Length")) {
      char *value = strchr(line, ':') + 1;
      char *end;
      unsigned long long parsed;
      while (*value == ' ' || *value == '\t')
        value++;
      if (*value == '-')
        return -1;
      errno = 0;
      parsed = strtoull(value, &end, 10);
      if (errno || end == value || *end || parsed > MAX_MESSAGE_SIZE ||
          have_length)
        return -1;
      content_length = (size_t)parsed;
      have_length = 1;
    }
  }

  if (!have_length)
    return -1;
  char *body = malloc(content_length + 1);
  if (!body)
    return -1;
  if (fread(body, 1, content_length, in) != content_length) {
    free(body);
    return -1;
  }
  body[content_length] = '\0';
  *out = body;
  *out_len = content_length;
  return 1;
}

static struct document *find_document(struct server *s, const char *uri) {
  for (struct document *d = s->documents; d; d = d->next) {
    if (strcmp(d->uri, uri) == 0)
      return d;
  }
  return NULL;
}

static int set_document(struct server *s, const char *uri, const char *text,
                        size_t text_len, long version, int has_version) {
  struct document *d = find_document(s, uri);
  char *new_text = malloc(text_len + 1);
  char *new_uri = NULL;

  if (!new_text)
    return 0;
  memcpy(new_text, text, text_len);
  new_text[text_len] = '\0';

  if (!d) {
    new_uri = malloc(strlen(uri) + 1);
    if (!new_uri) {
      free(new_text);
      return 0;
    }
    strcpy(new_uri, uri);
    d = calloc(1, sizeof *d);
    if (!d) {
      free(new_uri);
      free(new_text);
      return 0;
    }
    d->uri = new_uri;
    d->next = s->documents;
    s->documents = d;
  }

  free(d->text);
  d->text = new_text;
  d->text_len = text_len;
  d->version = version;
  d->has_version = has_version;
  return 1;
}

static void remove_document(struct server *s, const char *uri) {
  struct document **link = &s->documents;
  while (*link) {
    struct document *d = *link;
    if (strcmp(d->uri, uri) == 0) {
      *link = d->next;
      free(d->uri);
      free(d->text);
      free(d);
      return;
    }
    link = &d->next;
  }
}

static void free_documents(struct server *s) {
  while (s->documents) {
    struct document *d = s->documents;
    s->documents = d->next;
    free(d->uri);
    free(d->text);
    free(d);
  }
}

/* Decode one UTF-8 code point. Invalid sequences count as one replacement
 * character/code unit, allowing positions to remain bounded and monotonic. */
static size_t utf8_decode(const unsigned char *s, size_t len, unsigned *code) {
  unsigned c;
  if (!len)
    return 0;
  c = s[0];
  if (c < 0x80) {
    *code = c;
    return 1;
  }
  if (c >= 0xc2 && c <= 0xdf && len >= 2 && (s[1] & 0xc0) == 0x80) {
    *code = ((c & 0x1f) << 6) | (s[1] & 0x3f);
    return 2;
  }
  if (c >= 0xe0 && c <= 0xef && len >= 3 && (s[1] & 0xc0) == 0x80 &&
      (s[2] & 0xc0) == 0x80) {
    unsigned value = ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
    if (value >= 0x800 && !(value >= 0xd800 && value <= 0xdfff)) {
      *code = value;
      return 3;
    }
  }
  if (c >= 0xf0 && c <= 0xf4 && len >= 4 && (s[1] & 0xc0) == 0x80 &&
      (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
    unsigned value = ((c & 0x07) << 18) | ((s[1] & 0x3f) << 12) |
                     ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
    if (value >= 0x10000 && value <= 0x10ffff) {
      *code = value;
      return 4;
    }
  }
  *code = 0xfffd;
  return 1;
}

static void byte_offset_to_position(const char *text, size_t len, size_t offset,
                                    size_t *line, size_t *character) {
  size_t i = 0;
  size_t current_line = 0;
  size_t current_character = 0;
  if (offset > len)
    offset = len;

  while (i < offset) {
    unsigned code;
    size_t width;
    if (text[i] == '\n') {
      current_line++;
      current_character = 0;
      i++;
      continue;
    }
    width = utf8_decode((const unsigned char *)text + i, offset - i, &code);
    if (!width)
      break;
    current_character += code > 0xffff ? 2 : 1;
    i += width;
  }
  *line = current_line;
  *character = current_character;
}

static int publish_diagnostics(struct server *s, const struct document *doc) {
  struct arena arena;
  struct complain_list complains;
  struct program *program;
  struct strbuf b = {0};
  int ok = 1;

  arena_init(&arena);
  complain_init(&complains, &arena);
  program = parse(doc->text, doc->text_len, &arena, &complains);
  if (complains.count == 0)
    check(program, &complains);

  ok = sb_append(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
                     "publishDiagnostics\",\"params\":{\"uri\":") &&
       sb_json_string(&b, doc->uri);
  if (ok && doc->has_version)
    ok = sb_printf(&b, ",\"version\":%ld", doc->version);
  if (ok)
    ok = sb_append(&b, ",\"diagnostics\":[");

  int first = 1;
  for (const struct complaints *d = complains.head; ok && d; d = d->next) {
    size_t start = d->offset;
    size_t end;
    size_t start_line, start_character, end_line, end_character;
    if (start > doc->text_len)
      start = doc->text_len;
    end = d->length > doc->text_len - start ? doc->text_len : start + d->length;
    byte_offset_to_position(doc->text, doc->text_len, start, &start_line,
                            &start_character);
    byte_offset_to_position(doc->text, doc->text_len, end, &end_line,
                            &end_character);

    if (!first)
      ok = sb_append(&b, ",");
    first = 0;
    if (ok)
      ok = sb_printf(&b,
                     "{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                     "\"end\":{\"line\":%zu,\"character\":%zu}},"
                     "\"severity\":1,\"source\":\"lifeless\",\"message\":",
                     start_line, start_character, end_line, end_character) &&
           sb_json_string(&b, d->message) && sb_append(&b, "}");
  }
  if (ok)
    ok = sb_append(&b, "]}}");
  arena_free(&arena);

  if (!ok) {
    sb_destroy(&b);
    return 0;
  }
  return send_buffer(s, &b);
}

static int publish_empty_diagnostics(struct server *s, const char *uri) {
  struct strbuf b = {0};
  int ok =
      sb_append(
          &b,
          "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
          "\"params\":{\"uri\":") &&
      sb_json_string(&b, uri) && sb_append(&b, ",\"diagnostics\":[]}}");
  if (!ok) {
    sb_destroy(&b);
    return 0;
  }
  return send_buffer(s, &b);
}

static int token_is_string(const struct json_doc *doc, int token) {
  return token >= 0 && (size_t)token < doc->count &&
         doc->tokens[token].type == JSON_STRING;
}

static int token_is_container(const struct json_doc *doc, int token,
                              enum json_type type) {
  return token >= 0 && (size_t)token < doc->count &&
         doc->tokens[token].type == type;
}

static int handle_did_open(struct server *s, const struct json_doc *doc,
                           int params) {
  int td = json_object_get(doc, params, "textDocument");
  int uri_token = json_object_get(doc, td, "uri");
  int text_token = json_object_get(doc, td, "text");
  int version_token = json_object_get(doc, td, "version");
  char *uri = NULL;
  char *text = NULL;
  size_t text_len = 0;
  long version = 0;
  int has_version = json_number_to_long(doc, version_token, &version);
  int ok;

  if (!token_is_container(doc, td, JSON_OBJECT) ||
      !token_is_string(doc, uri_token) || !token_is_string(doc, text_token))
    return 1;
  uri = json_string_copy(doc, uri_token);
  text = json_string_copy_len(doc, text_token, &text_len);
  if (!uri || !text) {
    free(uri);
    free(text);
    return 0;
  }
  ok = set_document(s, uri, text, text_len, version, has_version);
  struct document *opened = ok ? find_document(s, uri) : NULL;
  free(uri);
  free(text);
  if (!ok || !opened)
    return 0;
  return publish_diagnostics(s, opened);
}

static int handle_did_change(struct server *s, const struct json_doc *doc,
                             int params) {
  int td = json_object_get(doc, params, "textDocument");
  int uri_token = json_object_get(doc, td, "uri");
  int version_token = json_object_get(doc, td, "version");
  int changes = json_object_get(doc, params, "contentChanges");
  char *uri;
  struct document *document;
  long version = 0;
  int has_version = json_number_to_long(doc, version_token, &version);

  if (!token_is_string(doc, uri_token) ||
      !token_is_container(doc, changes, JSON_ARRAY))
    return 1;
  uri = json_string_copy(doc, uri_token);
  if (!uri)
    return 0;
  document = find_document(s, uri);
  if (!document) {
    free(uri);
    return 1;
  }

  /* The server advertises full-document sync, so each change replaces the
   * complete buffer. If several arrive in one notification, the last wins. */
  int last_text = -1;
  for (size_t i = 0;; i++) {
    int change = json_array_get(doc, changes, i);
    if (change < 0)
      break;
    int text = json_object_get(doc, change, "text");
    if (token_is_string(doc, text))
      last_text = text;
  }
  if (last_text >= 0) {
    size_t text_len = 0;
    char *text = json_string_copy_len(doc, last_text, &text_len);
    if (!text) {
      free(uri);
      return 0;
    }
    int ok = set_document(s, uri, text, text_len, version, has_version);
    free(text);
    if (!ok) {
      free(uri);
      return 0;
    }
  }
  free(uri);
  return publish_diagnostics(s, document);
}

static int handle_did_close(struct server *s, const struct json_doc *doc,
                            int params) {
  int td = json_object_get(doc, params, "textDocument");
  int uri_token = json_object_get(doc, td, "uri");
  char *uri;
  if (!token_is_string(doc, uri_token))
    return 1;
  uri = json_string_copy(doc, uri_token);
  if (!uri)
    return 0;
  remove_document(s, uri);
  int ok = publish_empty_diagnostics(s, uri);
  free(uri);
  return ok;
}

struct completion_item {
  const char *label;
  int kind;
  const char *detail;
};

static int handle_completion(struct server *s, const struct json_doc *doc,
                             int params, const char *id, size_t id_len) {
  static const struct completion_item items[] = {
      {"die", 14, "Function declaration keyword"},
      {"try", 14, "Function declaration keyword"},
      {"ro", 14, "Read-only declaration keyword"},
      {"if", 14, "Conditional keyword"},
      {"else", 14, "Conditional keyword"},
      {"while", 14, "Loop keyword"},
      {"return", 14, "Return keyword"},
      {"true", 14, "Boolean literal"},
      {"false", 14, "Boolean literal"},
      {"none", 14, "None literal"},
      {"print", 3, "Built-in function"}};

  int td = json_object_get(doc, params, "textDocument");
  int uri_token = json_object_get(doc, td, "uri");

  if (!token_is_container(doc, params, JSON_OBJECT) ||
      !token_is_container(doc, td, JSON_OBJECT) ||
      !token_is_string(doc, uri_token))
    return send_error(s, id, id_len, -32602, "Invalid params");

  char *uri = json_string_copy(doc, uri_token);
  if (!uri)
    return 0;

  struct document *opened = find_document(s, uri);
  free(uri);

  /* Completion is only available for open documents. */
  if (!opened)
    return send_response(s, id, id_len, "[]");

  struct strbuf result = {0};
  int ok = sb_append(&result, "[");

  for (size_t i = 0; ok && i < sizeof items / sizeof items[0]; i++) {
    if (i)
      ok = sb_append(&result, ",");

    if (ok)
      ok = sb_append(&result, "{\"label\":") &&
           sb_json_string(&result, items[i].label) &&
           sb_printf(&result, ",\"kind\":%d,\"detail\":", items[i].kind) &&
           sb_json_string(&result, items[i].detail) && sb_append(&result, "}");
  }

  if (ok)
    ok = sb_append(&result, "]");

  if (!ok) {
    sb_destroy(&result);
    return 0;
  }

  ok = send_response(s, id, id_len, result.data);
  sb_destroy(&result);
  return ok;
}

static int handle_message(struct server *s, const char *message,
                          size_t message_len) {
  struct json_doc doc;
  int root;
  int method_token;
  int id_token;
  int params;
  char *method = NULL;
  const char *id_raw = "null";
  size_t id_len = 4;
  int has_id;
  int ok = 1;

  if (!json_parse(&doc, message, message_len))
    return send_error(s, "null", 4, -32700, "Parse error");
  root = 0;
  if (!token_is_container(&doc, root, JSON_OBJECT)) {
    json_destroy(&doc);
    return send_error(s, "null", 4, -32600, "Invalid Request");
  }

  int version = json_object_get(&doc, root, "jsonrpc");
  method_token = json_object_get(&doc, root, "method");
  id_token = json_object_get(&doc, root, "id");
  has_id = id_token >= 0;
  params = json_object_get(&doc, root, "params");

  /* Server-initiated responses are not expected, but are valid input and can
   * be ignored. */
  if (method_token < 0) {
    json_destroy(&doc);
    return 1;
  }
  if (!token_is_string(&doc, version) || !token_is_string(&doc, method_token)) {
    json_destroy(&doc);
    return send_error(s, "null", 4, -32600, "Invalid Request");
  }
  char *jsonrpc = json_string_copy(&doc, version);
  method = json_string_copy(&doc, method_token);
  if (!jsonrpc || strcmp(jsonrpc, "2.0") != 0 || !method) {
    free(jsonrpc);
    free(method);
    json_destroy(&doc);
    return send_error(s, "null", 4, -32600, "Invalid Request");
  }
  free(jsonrpc);

  if (has_id) {
    if (doc.tokens[id_token].type != JSON_STRING &&
        doc.tokens[id_token].type != JSON_NUMBER &&
        doc.tokens[id_token].type != JSON_NULL) {
      free(method);
      json_destroy(&doc);
      return send_error(s, "null", 4, -32600, "Invalid Request");
    }
    id_raw = json_raw(&doc, id_token, &id_len);
  }

  if (strcmp(method, "initialize") == 0) {
    static const char result[] =
        "{\"capabilities\":{"
        "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
        "\"completionProvider\":{\"resolveProvider\":false}"
        "},"
        "\"serverInfo\":{\"name\":\"lifeful\",\"version\":\"" LIFELESS_VERSION
        "\"}}";
    if (!has_id)
      ok = send_error(s, "null", 4, -32600, "Invalid Request");
    else
      ok = send_response(s, id_raw, id_len, result);
  } else if (strcmp(method, "initialized") == 0 ||
             strcmp(method, "$/setTrace") == 0) {
    /* Notifications with no server-side action. */
  } else if (strcmp(method, "textDocument/didOpen") == 0) {
    ok = handle_did_open(s, &doc, params);
  } else if (strcmp(method, "textDocument/didChange") == 0) {
    ok = handle_did_change(s, &doc, params);
  } else if (strcmp(method, "textDocument/didClose") == 0) {
    ok = handle_did_close(s, &doc, params);
  } else if (strcmp(method, "textDocument/completion") == 0) {
    if (has_id)
      ok = handle_completion(s, &doc, params, id_raw, id_len);
  } else if (strcmp(method, "shutdown") == 0) {
    if (has_id) {
      s->shutdown_requested = 1;
      ok = send_response(s, id_raw, id_len, "null");
    }
  } else if (strcmp(method, "exit") == 0) {
    ok = 2;
  } else if (has_id) {
    ok = send_error(s, id_raw, id_len, -32601, "Method not found");
  }

  free(method);
  json_destroy(&doc);
  return ok;
}

int lsp_server_run(FILE *input, FILE *output, FILE *errors) {
  struct server s = {.input = input,
                     .output = output,
                     .errors = errors,
                     .documents = NULL,
                     .shutdown_requested = 0};
  int exit_status = 1;

  for (;;) {
    char *message;
    size_t message_len;
    int read_status = read_message(s.input, &message, &message_len);
    if (read_status == 0) {
      exit_status = s.shutdown_requested ? 0 : 1;
      break;
    }
    if (read_status < 0) {
      fprintf(s.errors, "lifeful: invalid or incomplete LSP message\n");
      exit_status = 1;
      break;
    }

    int status = handle_message(&s, message, message_len);
    free(message);
    if (status == 2) {
      exit_status = s.shutdown_requested ? 0 : 1;
      break;
    }
    if (!status) {
      fprintf(s.errors, "lifeful: failed to process or write LSP message\n");
      exit_status = 1;
      break;
    }
  }

  free_documents(&s);
  return exit_status;
}
