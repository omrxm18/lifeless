/* lifeful - json.c
 * Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "json.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { JSON_MAX_DEPTH = 128 };

struct parser {
  struct json_doc *doc;
  size_t pos;
  size_t capacity;
};

static void skip_ws(struct parser *p) {
  while (p->pos < p->doc->source_len) {
    char c = p->doc->source[p->pos];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      break;
    p->pos++;
  }
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static int parse_string_end(struct parser *p, size_t *end) {
  const char *s = p->doc->source;
  size_t len = p->doc->source_len;

  if (p->pos >= len || s[p->pos] != '"')
    return 0;
  p->pos++;
  while (p->pos < len) {
    unsigned char c = (unsigned char)s[p->pos++];
    if (c == '"') {
      *end = p->pos;
      return 1;
    }
    if (c < 0x20)
      return 0;
    if (c != '\\')
      continue;
    if (p->pos >= len)
      return 0;
    c = (unsigned char)s[p->pos++];
    if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' ||
        c == 'r' || c == 't')
      continue;
    if (c != 'u' || p->pos + 4 > len)
      return 0;
    for (size_t i = 0; i < 4; i++) {
      if (hex_digit(s[p->pos + i]) < 0)
        return 0;
    }
    p->pos += 4;
  }
  return 0;
}

static int add_token(struct parser *p, enum json_type type, size_t start,
                     size_t end, size_t *index) {
  struct json_token *tokens;
  size_t capacity;

  if (p->doc->count == p->capacity) {
    capacity = p->capacity ? p->capacity * 2 : 64;
    if (capacity < p->capacity || capacity > SIZE_MAX / sizeof *tokens)
      return 0;
    tokens = realloc(p->doc->tokens, capacity * sizeof *tokens);
    if (!tokens)
      return 0;
    p->doc->tokens = tokens;
    p->capacity = capacity;
  }

  *index = p->doc->count++;
  p->doc->tokens[*index] = (struct json_token){
      .type = type, .start = start, .end = end, .next = *index + 1};
  return 1;
}

static int parse_value(struct parser *p, unsigned depth, size_t *index);

static int parse_container(struct parser *p, enum json_type type,
                           unsigned depth, size_t *index) {
  const char close = type == JSON_OBJECT ? '}' : ']';
  size_t start = p->pos;
  size_t container;

  if (!add_token(p, type, start, start, &container))
    return 0;
  p->pos++;
  skip_ws(p);

  if (p->pos < p->doc->source_len && p->doc->source[p->pos] == close) {
    p->pos++;
    p->doc->tokens[container].end = p->pos;
    p->doc->tokens[container].next = p->doc->count;
    *index = container;
    return 1;
  }

  for (;;) {
    size_t child;
    if (type == JSON_OBJECT) {
      size_t key_start = p->pos;
      size_t key_end;
      if (!parse_string_end(p, &key_end))
        return 0;
      if (!add_token(p, JSON_STRING, key_start, key_end, &child))
        return 0;
      p->doc->tokens[child].next = p->doc->count;
      skip_ws(p);
      if (p->pos >= p->doc->source_len || p->doc->source[p->pos] != ':')
        return 0;
      p->pos++;
      skip_ws(p);
      if (!parse_value(p, depth + 1, &child))
        return 0;
    } else {
      if (!parse_value(p, depth + 1, &child))
        return 0;
    }

    skip_ws(p);
    if (p->pos >= p->doc->source_len)
      return 0;
    if (p->doc->source[p->pos] == close) {
      p->pos++;
      p->doc->tokens[container].end = p->pos;
      p->doc->tokens[container].next = p->doc->count;
      *index = container;
      return 1;
    }
    if (p->doc->source[p->pos] != ',')
      return 0;
    p->pos++;
    skip_ws(p);
  }
}

static int parse_number(struct parser *p, size_t *index) {
  const char *s = p->doc->source;
  size_t len = p->doc->source_len;
  size_t start = p->pos;

  if (p->pos < len && s[p->pos] == '-')
    p->pos++;
  if (p->pos >= len)
    return 0;
  if (s[p->pos] == '0') {
    p->pos++;
  } else {
    if (s[p->pos] < '1' || s[p->pos] > '9')
      return 0;
    while (p->pos < len && s[p->pos] >= '0' && s[p->pos] <= '9')
      p->pos++;
  }
  if (p->pos < len && s[p->pos] == '.') {
    p->pos++;
    size_t digits = p->pos;
    while (p->pos < len && s[p->pos] >= '0' && s[p->pos] <= '9')
      p->pos++;
    if (p->pos == digits)
      return 0;
  }
  if (p->pos < len && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
    p->pos++;
    if (p->pos < len && (s[p->pos] == '+' || s[p->pos] == '-'))
      p->pos++;
    size_t digits = p->pos;
    while (p->pos < len && s[p->pos] >= '0' && s[p->pos] <= '9')
      p->pos++;
    if (p->pos == digits)
      return 0;
  }
  return add_token(p, JSON_NUMBER, start, p->pos, index);
}

static int parse_value(struct parser *p, unsigned depth, size_t *index) {
  const char *s = p->doc->source;
  size_t len = p->doc->source_len;
  size_t start;

  if (depth > JSON_MAX_DEPTH)
    return 0;
  skip_ws(p);
  if (p->pos >= len)
    return 0;
  start = p->pos;

  if (s[p->pos] == '{')
    return parse_container(p, JSON_OBJECT, depth, index);
  if (s[p->pos] == '[')
    return parse_container(p, JSON_ARRAY, depth, index);
  if (s[p->pos] == '"') {
    size_t end;
    if (!parse_string_end(p, &end))
      return 0;
    p->pos = end;
    return add_token(p, JSON_STRING, start, end, index);
  }
  if (s[p->pos] == '-' || (s[p->pos] >= '0' && s[p->pos] <= '9'))
    return parse_number(p, index);

  struct {
    const char *text;
    size_t length;
    enum json_type type;
  } literals[] = {
      {"true", 4, JSON_TRUE}, {"false", 5, JSON_FALSE}, {"null", 4, JSON_NULL}};
  for (size_t i = 0; i < sizeof literals / sizeof literals[0]; i++) {
    if (len - p->pos >= literals[i].length &&
        memcmp(s + p->pos, literals[i].text, literals[i].length) == 0) {
      p->pos += literals[i].length;
      return add_token(p, literals[i].type, start, p->pos, index);
    }
  }
  return 0;
}

int json_parse(struct json_doc *doc, const char *source, size_t source_len) {
  struct parser p = {0};
  size_t root;

  memset(doc, 0, sizeof *doc);
  doc->source = source;
  doc->source_len = source_len;
  p.doc = doc;
  if (!parse_value(&p, 0, &root)) {
    json_destroy(doc);
    return 0;
  }
  skip_ws(&p);
  if (p.pos != source_len || root != 0) {
    json_destroy(doc);
    return 0;
  }
  return 1;
}

void json_destroy(struct json_doc *doc) {
  free(doc->tokens);
  memset(doc, 0, sizeof *doc);
}

static int string_token_equals(const struct json_doc *doc, int token,
                               const char *expected) {
  char *value = json_string_copy(doc, token);
  int equal = value && strcmp(value, expected) == 0;
  free(value);
  return equal;
}

int json_object_get(const struct json_doc *doc, int object, const char *name) {
  size_t i;
  if (object < 0 || (size_t)object >= doc->count ||
      doc->tokens[object].type != JSON_OBJECT)
    return -1;

  i = (size_t)object + 1;
  while (i < doc->tokens[object].next) {
    size_t key = i;
    size_t value = key + 1;
    if (value >= doc->tokens[object].next)
      return -1;
    if (doc->tokens[key].type == JSON_STRING &&
        string_token_equals(doc, (int)key, name))
      return (int)value;
    i = doc->tokens[value].next;
  }
  return -1;
}

int json_array_get(const struct json_doc *doc, int array, size_t index) {
  size_t i;
  if (array < 0 || (size_t)array >= doc->count ||
      doc->tokens[array].type != JSON_ARRAY)
    return -1;
  i = (size_t)array + 1;
  while (i < doc->tokens[array].next) {
    if (index == 0)
      return (int)i;
    i = doc->tokens[i].next;
    index--;
  }
  return -1;
}

const char *json_raw(const struct json_doc *doc, int token, size_t *length) {
  if (token < 0 || (size_t)token >= doc->count)
    return NULL;
  if (length)
    *length = doc->tokens[token].end - doc->tokens[token].start;
  return doc->source + doc->tokens[token].start;
}

static int append_utf8(char *out, size_t cap, size_t *used, unsigned code) {
  unsigned char bytes[4];
  size_t n;
  if (code <= 0x7f) {
    bytes[0] = (unsigned char)code;
    n = 1;
  } else if (code <= 0x7ff) {
    bytes[0] = (unsigned char)(0xc0 | (code >> 6));
    bytes[1] = (unsigned char)(0x80 | (code & 0x3f));
    n = 2;
  } else if (code <= 0xffff) {
    bytes[0] = (unsigned char)(0xe0 | (code >> 12));
    bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | (code & 0x3f));
    n = 3;
  } else if (code <= 0x10ffff) {
    bytes[0] = (unsigned char)(0xf0 | (code >> 18));
    bytes[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    bytes[3] = (unsigned char)(0x80 | (code & 0x3f));
    n = 4;
  } else {
    return 0;
  }
  if (*used > cap || n > cap - *used)
    return 0;
  memcpy(out + *used, bytes, n);
  *used += n;
  return 1;
}

static int read_u16(const char *s, unsigned *value) {
  unsigned result = 0;
  for (size_t i = 0; i < 4; i++) {
    int h = hex_digit(s[i]);
    if (h < 0)
      return 0;
    result = (result << 4) | (unsigned)h;
  }
  *value = result;
  return 1;
}

char *json_string_copy_len(const struct json_doc *doc, int token,
                           size_t *decoded_length) {
  size_t start, end, used = 0;
  char *out;
  const char *s;

  if (token < 0 || (size_t)token >= doc->count ||
      doc->tokens[token].type != JSON_STRING)
    return NULL;
  start = doc->tokens[token].start + 1;
  end = doc->tokens[token].end - 1;
  out = malloc(end - start + 1);
  if (!out)
    return NULL;
  s = doc->source;

  for (size_t i = start; i < end; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c != '\\') {
      out[used++] = (char)c;
      continue;
    }
    c = (unsigned char)s[++i];
    switch (c) {
    case '"':
      out[used++] = '"';
      break;
    case '\\':
      out[used++] = '\\';
      break;
    case '/':
      out[used++] = '/';
      break;
    case 'b':
      out[used++] = '\b';
      break;
    case 'f':
      out[used++] = '\f';
      break;
    case 'n':
      out[used++] = '\n';
      break;
    case 'r':
      out[used++] = '\r';
      break;
    case 't':
      out[used++] = '\t';
      break;
    case 'u': {
      unsigned code;
      if (i + 4 >= end || !read_u16(s + i + 1, &code)) {
        free(out);
        return NULL;
      }
      i += 4;
      if (code >= 0xd800 && code <= 0xdbff) {
        unsigned low;
        if (i + 6 >= end || s[i + 1] != '\\' || s[i + 2] != 'u' ||
            !read_u16(s + i + 3, &low) || low < 0xdc00 || low > 0xdfff) {
          free(out);
          return NULL;
        }
        i += 6;
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
      } else if (code >= 0xdc00 && code <= 0xdfff) {
        free(out);
        return NULL;
      }
      if (!append_utf8(out, end - start, &used, code)) {
        free(out);
        return NULL;
      }
      break;
    }
    default:
      free(out);
      return NULL;
    }
  }
  out[used] = '\0';
  if (decoded_length)
    *decoded_length = used;
  return out;
}

char *json_string_copy(const struct json_doc *doc, int token) {
  return json_string_copy_len(doc, token, NULL);
}

int json_number_to_long(const struct json_doc *doc, int token, long *value) {
  char *raw;
  char *end;
  long n;
  size_t length;
  const char *source = json_raw(doc, token, &length);
  if (!source || doc->tokens[token].type != JSON_NUMBER ||
      memchr(source, '.', length) || memchr(source, 'e', length) ||
      memchr(source, 'E', length))
    return 0;
  raw = malloc(length + 1);
  if (!raw)
    return 0;
  memcpy(raw, source, length);
  raw[length] = '\0';
  errno = 0;
  n = strtol(raw, &end, 10);
  if (errno || end != raw + length) {
    free(raw);
    return 0;
  }
  free(raw);
  *value = n;
  return 1;
}
