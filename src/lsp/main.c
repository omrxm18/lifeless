/* lifeful - main.c
 * Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "server.h"

static void print_usage(FILE *out) { fprintf(out, "usage: lifeful --stdio\n"); }

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--stdio") == 0)
    return lsp_server_run(stdin, stdout, stderr);

  if (argc == 2 && strcmp(argv[1], "--help") == 0) {
    print_usage(stdout);
    return 0;
  }

  print_usage(stderr);
  return 2;
}
