/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <wt_internal.h>

typedef struct {
	void   *mem;				/* Managed memory chunk */
	size_t	memsize;			/* Managed memory size */
} ULINE;

extern const char *home;			/* Home directory */
extern const char *progname;			/* Program name */
extern const char *usage_prefix;		/* Global arguments */
extern bool verbose;				/* Verbose flag */

extern WT_EVENT_HANDLER *verbose_handler;

extern int   __wt_opterr;		/* if error message should be printed */
extern int   __wt_optind;		/* index into parent argv vector */
extern int   __wt_optopt;		/* character checked for validity */
extern int   __wt_optreset;		/* reset getopt */
extern char *__wt_optarg;		/* argument associated with option */

int	 util_backup(WT_SESSION *, int, char *[]);
int	 util_cerr(WT_CURSOR *, const char *, int);
int	 util_compact(WT_SESSION *, int, char *[]);
void	 util_copyright(void);
int	 util_create(WT_SESSION *, int, char *[]);
int	 util_drop(WT_SESSION *, int, char *[]);
int	 util_dump(WT_SESSION *, int, char *[]);
int	 util_err(WT_SESSION *, int, const char *, ...);
int	 util_flush(WT_SESSION *, const char *);
int	 util_list(WT_SESSION *, int, char *[]);
int	 util_load(WT_SESSION *, int, char *[]);
int	 util_loadtext(WT_SESSION *, int, char *[]);
char	*util_name(WT_SESSION *, const char *, const char *);
int	 util_printlog(WT_SESSION *, int, char *[]);
int	 util_read(WT_SESSION *, int, char *[]);
int	 util_read_line(WT_SESSION *, ULINE *, bool, bool *);
int	 util_rebalance(WT_SESSION *, int, char *[]);
int	 util_rename(WT_SESSION *, int, char *[]);
int	 util_salvage(WT_SESSION *, int, char *[]);
int	 util_stat(WT_SESSION *, int, char *[]);
int	 util_str2recno(WT_SESSION *, const char *p, uint64_t *recnop);
int	 util_upgrade(WT_SESSION *, int, char *[]);
int	 util_verify(WT_SESSION *, int, char *[]);
int	 util_write(WT_SESSION *, int, char *[]);
