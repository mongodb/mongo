/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "test_util.h"

#include <signal.h>

#define	URI_BASE	"table:__wt"		/* File name */

#define	ERR_KEY_MISMATCH	0x200001
#define	ERR_DATA_MISMATCH	0x200002

/*
 * There are three different table types in the test, and a 'special' type
 * of mixed (i.e a mixture of the other three types.
 */
#define	MAX_TABLE_TYPE	3
typedef enum { MIX = 0, COL, LSM, ROW } table_type;	/* File type */

/*
 * Per-table cookie structure.
 */
typedef struct {
	int id;
	table_type type;			/* Type for table. */
	char uri[128];
} COOKIE;

typedef struct {
	char *home;				/* Home directory */
	const char *checkpoint_name;		/* Checkpoint name */
	WT_CONNECTION *conn;			/* WiredTiger connection */
	u_int nkeys;				/* Keys to load */
	u_int nops;				/* Operations per thread */
	FILE *logfp;				/* Message log file. */
	char *progname;				/* Program name */
	int nworkers;				/* Number workers configured */
	int ntables;				/* Number tables configured */
	int ntables_created;			/* Number tables opened */
	int running;				/* Whether to stop */
	int status;				/* Exit status */
	COOKIE *cookies;			/* Per-thread info */
	pthread_t checkpoint_thread;		/* Checkpoint thread */
} GLOBAL;
extern GLOBAL g;

int end_checkpoints(void);
int log_print_err(const char *, int, int);
int start_checkpoints(void);
int start_workers(table_type);
const char *type_to_string(table_type);
