/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

struct __wt_config {
	WT_SESSION_IMPL *session;
	const char *orig;
	const char *end;
	const char *cur;

	int depth, top;
	const int8_t *go;
};

struct __wt_config_item {
	const char *str;
	size_t len;
	int64_t val;
	enum { ITEM_STRING, ITEM_BOOL, ITEM_ID, ITEM_NUM, ITEM_STRUCT } type;
};

struct __wt_config_check {
	const char *name;
	const char *type;
	const char *checks;
	const WT_CONFIG_CHECK *subconfigs;
};

#define	WT_CONFIG_REF(session, n)					\
	(S2C(session)->config_entries[WT_CONFIG_ENTRY_##n])
struct __wt_config_entry {
	const char *method;			/* method name */

#define	WT_CONFIG_BASE(session, n)	(WT_CONFIG_REF(session, n)->base)
	const char *base;			/* configuration base */

	const WT_CONFIG_CHECK *checks;		/* check array */
};

/*
 * DO NOT EDIT: automatically built by dist/api_config.py.
 * configuration section: BEGIN
 */
#define	WT_CONFIG_ENTRY_colgroup_meta			 0
#define	WT_CONFIG_ENTRY_connection_add_collator		 1
#define	WT_CONFIG_ENTRY_connection_add_compressor	 2
#define	WT_CONFIG_ENTRY_connection_add_data_source	 3
#define	WT_CONFIG_ENTRY_connection_add_extractor	 4
#define	WT_CONFIG_ENTRY_connection_close		 5
#define	WT_CONFIG_ENTRY_connection_load_extension	 6
#define	WT_CONFIG_ENTRY_connection_open_session		 7
#define	WT_CONFIG_ENTRY_connection_reconfigure		 8
#define	WT_CONFIG_ENTRY_cursor_close			 9
#define	WT_CONFIG_ENTRY_file_meta			10
#define	WT_CONFIG_ENTRY_index_meta			11
#define	WT_CONFIG_ENTRY_session_begin_transaction	12
#define	WT_CONFIG_ENTRY_session_checkpoint		13
#define	WT_CONFIG_ENTRY_session_close			14
#define	WT_CONFIG_ENTRY_session_commit_transaction	15
#define	WT_CONFIG_ENTRY_session_compact			16
#define	WT_CONFIG_ENTRY_session_create			17
#define	WT_CONFIG_ENTRY_session_drop			18
#define	WT_CONFIG_ENTRY_session_log_printf		19
#define	WT_CONFIG_ENTRY_session_open_cursor		20
#define	WT_CONFIG_ENTRY_session_reconfigure		21
#define	WT_CONFIG_ENTRY_session_rename			22
#define	WT_CONFIG_ENTRY_session_rollback_transaction	23
#define	WT_CONFIG_ENTRY_session_salvage			24
#define	WT_CONFIG_ENTRY_session_truncate		25
#define	WT_CONFIG_ENTRY_session_upgrade			26
#define	WT_CONFIG_ENTRY_session_verify			27
#define	WT_CONFIG_ENTRY_table_meta			28
#define	WT_CONFIG_ENTRY_wiredtiger_open			29
/*
 * configuration section: END
 * DO NOT EDIT: automatically built by dist/flags.py.
 */
