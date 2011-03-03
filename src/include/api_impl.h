/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

typedef struct ICURSOR_TABLE {
	WT_CURSOR_STD cstd;

	DB *db;
	WT_WALK walk;
	WT_REF *ref;
	WT_COL *cip;
	WT_ROW *rip;
	WT_SCRATCH *key_tmp, *value_tmp;
	uint32_t nitems;
} ICURSOR_TABLE;

/*
 * WT_STACK --
 *	We maintain a stack of parent pages as we build the tree, encapsulated
 *	in this structure.
 */
typedef struct {
	WT_PAGE	*page;				/* page header */
	uint8_t	*first_free;			/* page's first free byte */
	uint32_t space_avail;			/* page's space available */

	WT_SCRATCH *tmp;			/* page-in-a-buffer */
	void *data;				/* last on-page WT_COL/WT_ROW */
} WT_STACK_ELEM;

typedef struct {
	WT_STACK_ELEM *elem;			/* stack */
	u_int size;				/* stack size */
} WT_STACK;

typedef struct ICURSOR_BULK {
	ICURSOR_TABLE ctable;

	WT_SCRATCH *tmp;
	WT_PAGE *page;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t space_avail;
	uint8_t *first_free;
} ICURSOR_BULK;

typedef struct ISESSION {
	WT_SESSION iface;

	WT_ERROR_HANDLER *error_handler;

	WT_TOC *toc;

	TAILQ_ENTRY(ISESSION) q;
	TAILQ_HEAD(__cursors, WT_CURSOR_STD) cursors;

	TAILQ_HEAD(__btrees, __db) btrees;
} ISESSION;

typedef struct ICONNECTION {
	WT_CONNECTION iface;

	ENV *env;
	const char *home;

	TAILQ_HEAD(__sessions, ISESSION) sessions;
} ICONNECTION;

extern WT_ERROR_HANDLER *__wt_error_handler_default;
