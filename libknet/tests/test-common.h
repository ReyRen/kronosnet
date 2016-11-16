/*
 * Copyright (C) 2016 Red Hat, Inc.  All rights reserved.
 *
 * Authors: Fabio M. Di Nitto <fabbione@kronosnet.org>
 *
 * This software licensed under GPL-2.0+, LGPL-2.0+
 */

#ifndef __TEST_COMMON_H__
#define __TEST_COMMON_H__

/*
 * error codes from automake test-driver
 */

#define PASS	0
#define SKIP	77
#define ERROR	99
#define FAIL	-1

/*
 * common facilities
 */

int execute_shell(const char *command, char **error_string);

int is_memcheck(void);
int is_helgrind(void);

int need_root(void);

/*
 * high level logging function.
 * automatically setup logpipes and start/stop logging thread.
 *
 * start_logging exit(FAIL) on error or fd to pass to knet_handle_new
 * and it will install an atexit handle to close logging properly
 */
int start_logging(struct _IO_FILE *std);

int setup_logpipes(int *logfds);
void close_logpipes(int *logfds);
void flush_logs(int logfd, struct _IO_FILE *std);
int start_logthread(int logfd, struct _IO_FILE *std);
int stop_logthread(void);

#endif
