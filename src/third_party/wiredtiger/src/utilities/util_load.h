/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * A list of configuration strings.
 */
typedef struct {
	char **list;		/* array of alternating (uri, config) values */
	int entry;		/* next entry available in list */
	int max_entry;		/* how many allocated in list */
} CONFIG_LIST;

int	 config_exec(WT_SESSION *, char **);
int	 config_list_add(WT_SESSION *, CONFIG_LIST *, char *);
void	 config_list_free(CONFIG_LIST *);
int	 config_reorder(WT_SESSION *, char **);
int	 config_update(WT_SESSION *, char **);

/* Flags for util_load_json */
#define	LOAD_JSON_APPEND	0x0001	/* append (ignore record number keys) */
#define	LOAD_JSON_NO_OVERWRITE	0x0002	/* don't overwrite existing data */

int	 util_load_json(WT_SESSION *, const char *, uint32_t);
