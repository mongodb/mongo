/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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

struct __wt_config_check {
	const char *name;
	const char *type;
	int (*checkf)(WT_SESSION_IMPL *, WT_CONFIG_ITEM *);
	const char *checks;
	const WT_CONFIG_CHECK *subconfigs;
	u_int subconfigs_entries;
};

#define	WT_CONFIG_REF(session, n)					\
	(S2C(session)->config_entries[WT_CONFIG_ENTRY_##n])
struct __wt_config_entry {
	const char *method;			/* method name */

#define	WT_CONFIG_BASE(session, n)	(WT_CONFIG_REF(session, n)->base)
	const char *base;			/* configuration base */

	const WT_CONFIG_CHECK *checks;		/* check array */
	u_int checks_entries;
};

struct __wt_config_parser_impl {
	WT_CONFIG_PARSER iface;

	WT_SESSION_IMPL *session;
	WT_CONFIG config;
	WT_CONFIG_ITEM config_item;
};

/*
 * DO NOT EDIT: automatically built by dist/api_config.py.
 * configuration section: BEGIN
 */
#define	WT_CONFIG_ENTRY_WT_CONNECTION_add_collator	 0
#define	WT_CONFIG_ENTRY_WT_CONNECTION_add_compressor	 1
#define	WT_CONFIG_ENTRY_WT_CONNECTION_add_data_source	 2
#define	WT_CONFIG_ENTRY_WT_CONNECTION_add_encryptor	 3
#define	WT_CONFIG_ENTRY_WT_CONNECTION_add_extractor	 4
#define	WT_CONFIG_ENTRY_WT_CONNECTION_async_new_op	 5
#define	WT_CONFIG_ENTRY_WT_CONNECTION_close		 6
#define	WT_CONFIG_ENTRY_WT_CONNECTION_load_extension	 7
#define	WT_CONFIG_ENTRY_WT_CONNECTION_open_session	 8
#define	WT_CONFIG_ENTRY_WT_CONNECTION_reconfigure	 9
#define	WT_CONFIG_ENTRY_WT_CURSOR_close			10
#define	WT_CONFIG_ENTRY_WT_CURSOR_reconfigure		11
#define	WT_CONFIG_ENTRY_WT_SESSION_begin_transaction	12
#define	WT_CONFIG_ENTRY_WT_SESSION_checkpoint		13
#define	WT_CONFIG_ENTRY_WT_SESSION_close		14
#define	WT_CONFIG_ENTRY_WT_SESSION_commit_transaction	15
#define	WT_CONFIG_ENTRY_WT_SESSION_compact		16
#define	WT_CONFIG_ENTRY_WT_SESSION_create		17
#define	WT_CONFIG_ENTRY_WT_SESSION_drop			18
#define	WT_CONFIG_ENTRY_WT_SESSION_log_printf		19
#define	WT_CONFIG_ENTRY_WT_SESSION_open_cursor		20
#define	WT_CONFIG_ENTRY_WT_SESSION_reconfigure		21
#define	WT_CONFIG_ENTRY_WT_SESSION_rename		22
#define	WT_CONFIG_ENTRY_WT_SESSION_rollback_transaction	23
#define	WT_CONFIG_ENTRY_WT_SESSION_salvage		24
#define	WT_CONFIG_ENTRY_WT_SESSION_snapshot		25
#define	WT_CONFIG_ENTRY_WT_SESSION_strerror		26
#define	WT_CONFIG_ENTRY_WT_SESSION_transaction_sync	27
#define	WT_CONFIG_ENTRY_WT_SESSION_truncate		28
#define	WT_CONFIG_ENTRY_WT_SESSION_upgrade		29
#define	WT_CONFIG_ENTRY_WT_SESSION_verify		30
#define	WT_CONFIG_ENTRY_colgroup_meta			31
#define	WT_CONFIG_ENTRY_file_meta			32
#define	WT_CONFIG_ENTRY_index_meta			33
#define	WT_CONFIG_ENTRY_table_meta			34
#define	WT_CONFIG_ENTRY_wiredtiger_open			35
#define	WT_CONFIG_ENTRY_wiredtiger_open_all		36
#define	WT_CONFIG_ENTRY_wiredtiger_open_basecfg		37
#define	WT_CONFIG_ENTRY_wiredtiger_open_usercfg		38
/*
 * configuration section: END
 * DO NOT EDIT: automatically built by dist/flags.py.
 */
