/* lifeful - json.h
 * Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef LIFEFUL_JSON_H
#define LIFEFUL_JSON_H

#include <stddef.h>

enum json_type {
  JSON_OBJECT,
  JSON_ARRAY,
  JSON_STRING,
  JSON_NUMBER,
  JSON_TRUE,
  JSON_FALSE,
  JSON_NULL
};

struct json_token {
  enum json_type type;
  size_t start;
  size_t end;
  size_t next; /* first token after this token's complete subtree */
};

struct json_doc {
  const char *source;
  size_t source_len;
  struct json_token *tokens;
  size_t count;
};

/* Parse one complete JSON value. Returns 1 on success, 0 on invalid JSON. */
int json_parse(struct json_doc *doc, const char *source, size_t source_len);
void json_destroy(struct json_doc *doc);

/* Return a token index, or -1 when absent / of the wrong container type. */
int json_object_get(const struct json_doc *doc, int object,
                    const char *name);
int json_array_get(const struct json_doc *doc, int array, size_t index);

/* Decode a JSON string token to UTF-8. The returned allocation is caller-owned. */
char *json_string_copy(const struct json_doc *doc, int token);
char *json_string_copy_len(const struct json_doc *doc, int token,
                           size_t *decoded_length);

/* Return the token's original JSON representation (not NUL-terminated). */
const char *json_raw(const struct json_doc *doc, int token, size_t *length);

/* Convert a JSON number token to long. Returns 1 on success. */
int json_number_to_long(const struct json_doc *doc, int token, long *value);

#endif
