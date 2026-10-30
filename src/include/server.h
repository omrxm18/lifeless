/* lifeful - server.h
 * Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef LIFEFUL_SERVER_H
#define LIFEFUL_SERVER_H

#include <stdio.h>

/* Run the Language Server Protocol over the supplied stdio streams. */
int lsp_server_run(FILE *input, FILE *output, FILE *errors);

#endif
