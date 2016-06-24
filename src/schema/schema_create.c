/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_direct_io_size_check --
 *	Return a size from the configuration, complaining if it's insufficient
 * for direct I/O.
 */
int
__wt_direct_io_size_check(WT_SESSION_IMPL *session,
    const char **cfg, const char *config_name, uint32_t *allocsizep)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	int64_t align;

	*allocsizep = 0;

	conn = S2C(session);

	WT_RET(__wt_config_gets(session, cfg, config_name, &cval));

	/*
	 * This function exists as a place to hang this comment: if direct I/O
	 * is configured, page sizes must be at least as large as any buffer
	 * alignment as well as a multiple of the alignment.  Linux gets unhappy
	 * if you configure direct I/O and then don't do I/O in alignments and
	 * units of its happy place.
	 */
	if (FLD_ISSET(conn->direct_io,
	   WT_DIRECT_IO_CHECKPOINT | WT_DIRECT_IO_DATA)) {
		align = (int64_t)conn->buffer_alignment;
		if (align != 0 && (cval.val < align || cval.val % align != 0))
			WT_RET_MSG(session, EINVAL,
			    "when direct I/O is configured, the %s size must "
			    "be at least as large as the buffer alignment as "
			    "well as a multiple of the buffer alignment",
			    config_name);
	}
	*allocsizep = (uint32_t)cval.val;
	return (0);
}

/*
 * __create_file --
 *	Create a new 'file:' object.
 */
static int
__create_file(WT_SESSION_IMPL *session,
    const char *uri, bool exclusive, const char *config)
{
	WT_DECL_ITEM(val);
	WT_DECL_RET;
	const char *filename, **p, *filecfg[] =
	    { WT_CONFIG_BASE(session, file_meta), config, NULL, NULL };
	char *fileconf;
	uint32_t allocsize;
	bool is_metadata;

	fileconf = NULL;

	is_metadata = strcmp(uri, WT_METAFILE_URI) == 0;

	filename = uri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(session, EINVAL, "Expected a 'file:' URI: %s", uri);

	/* Check if the file already exists. */
	if (!is_metadata && (ret =
	    __wt_metadata_search(session, uri, &fileconf)) != WT_NOTFOUND) {
		if (exclusive)
			WT_TRET(EEXIST);
		goto err;
	}

	/* Sanity check the allocation size. */
	WT_ERR(__wt_direct_io_size_check(
	    session, filecfg, "allocation_size", &allocsize));

	/* Create the file. */
	WT_ERR(__wt_block_manager_create(session, filename, allocsize));
	if (WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_fileop(session, NULL, uri));

	/*
	 * If creating an ordinary file, append the file ID and current version
	 * numbers to the passed-in configuration and insert the resulting
	 * configuration into the metadata.
	 */
	if (!is_metadata) {
		WT_ERR(__wt_scr_alloc(session, 0, &val));
		WT_ERR(__wt_buf_fmt(session, val,
		    "id=%" PRIu32 ",version=(major=%d,minor=%d)",
		    ++S2C(session)->next_file_id,
		    WT_BTREE_MAJOR_VERSION_MAX, WT_BTREE_MINOR_VERSION_MAX));
		for (p = filecfg; *p != NULL; ++p)
			;
		*p = val->data;
		WT_ERR(__wt_config_collapse(session, filecfg, &fileconf));
		WT_ERR(__wt_metadata_insert(session, uri, fileconf));
	}

	/*
	 * Open the file to check that it was setup correctly. We don't need to
	 * pass the configuration, we just wrote the collapsed configuration
	 * into the metadata file, and it's going to be read/used by underlying
	 * functions.
	 *
	 * Keep the handle exclusive until it is released at the end of the
	 * call, otherwise we could race with a drop.
	 */
	WT_ERR(__wt_session_get_btree(
	    session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
	if (WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_handle_lock(session, true));
	else
		WT_ERR(__wt_session_release_btree(session));

err:	__wt_scr_free(session, &val);
	__wt_free(session, fileconf);
	return (ret);
}

/*
 * __wt_schema_colgroup_source --
 *	Get the URI of the data source for a column group.
 */
int
__wt_schema_colgroup_source(WT_SESSION_IMPL *session,
    WT_TABLE *table, const char *cgname, const char *config, WT_ITEM *buf)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	size_t len;
	const char *prefix, *suffix, *tablename;

	tablename = table->name + strlen("table:");
	if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
	    !WT_STRING_MATCH("file", cval.str, cval.len)) {
		prefix = cval.str;
		len = cval.len;
		suffix = "";
	} else {
		prefix = "file";
		len = strlen(prefix);
		suffix = ".wt";
	}
	WT_RET_NOTFOUND_OK(ret);

	if (cgname == NULL)
		WT_RET(__wt_buf_fmt(session, buf, "%.*s:%s%s",
		    (int)len, prefix, tablename, suffix));
	else
		WT_RET(__wt_buf_fmt(session, buf, "%.*s:%s_%s%s",
		    (int)len, prefix, tablename, cgname, suffix));

	return (0);
}

/*
 * __create_colgroup --
 *	Create a column group.
 */
static int
__create_colgroup(WT_SESSION_IMPL *session,
    const char *name, bool exclusive, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_ITEM confbuf, fmt, namebuf;
	WT_TABLE *table;
	size_t tlen;
	const char **cfgp, *cfg[4] =
	    { WT_CONFIG_BASE(session, colgroup_meta), config, NULL, NULL };
	const char *sourcecfg[] = { config, NULL, NULL };
	const char *cgname, *source, *sourceconf, *tablename;
	char *cgconf, *origconf;
	bool exists;

	sourceconf = NULL;
	cgconf = origconf = NULL;
	WT_CLEAR(fmt);
	WT_CLEAR(confbuf);
	WT_CLEAR(namebuf);
	exists = false;

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "colgroup:"))
		return (EINVAL);
	cgname = strchr(tablename, ':');
	if (cgname != NULL) {
		tlen = (size_t)(cgname - tablename);
		++cgname;
	} else
		tlen = strlen(tablename);

	if ((ret =
	    __wt_schema_get_table(session, tablename, tlen, true, &table)) != 0)
		WT_RET_MSG(session, (ret == WT_NOTFOUND) ? ENOENT : ret,
		    "Can't create '%s' for non-existent table '%.*s'",
		    name, (int)tlen, tablename);

	/* Make sure the column group is referenced from the table. */
	if (cgname != NULL && (ret =
	    __wt_config_subgets(session, &table->cgconf, cgname, &cval)) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "Column group '%s' not found in table '%.*s'",
		    cgname, (int)tlen, tablename);

	/* Check if the column group already exists. */
	if ((ret = __wt_metadata_search(session, name, &origconf)) == 0) {
		if (exclusive)
			WT_ERR(EEXIST);
		exists = true;
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Find the first NULL entry in the cfg stack. */
	for (cfgp = &cfg[1]; *cfgp; cfgp++)
		;

	/* Add the source to the colgroup config before collapsing. */
	if (__wt_config_getones(
	    session, config, "source", &cval) == 0 && cval.len != 0) {
		WT_ERR(__wt_buf_fmt(
		    session, &namebuf, "%.*s", (int)cval.len, cval.str));
		source = namebuf.data;
	} else {
		WT_ERR(__wt_schema_colgroup_source(
		    session, table, cgname, config, &namebuf));
		source = namebuf.data;
		WT_ERR(__wt_buf_fmt(
		    session, &confbuf, "source=\"%s\"", source));
		*cfgp++ = confbuf.data;
	}

	/* Calculate the key/value formats: these go into the source config. */
	WT_ERR(__wt_buf_fmt(session, &fmt, "key_format=%s", table->key_format));
	if (cgname == NULL)
		WT_ERR(__wt_buf_catfmt
		    (session, &fmt, ",value_format=%s", table->value_format));
	else {
		if (__wt_config_getones(session, config, "columns", &cval) != 0)
			WT_ERR_MSG(session, EINVAL,
			    "No 'columns' configuration for '%s'", name);
		WT_ERR(__wt_buf_catfmt(session, &fmt, ",value_format="));
		WT_ERR(__wt_struct_reformat(session,
		    table, cval.str, cval.len, NULL, true, &fmt));
	}
	sourcecfg[1] = fmt.data;
	WT_ERR(__wt_config_merge(session, sourcecfg, NULL, &sourceconf));
	WT_ERR(__wt_schema_create(session, source, sourceconf));

	WT_ERR(__wt_config_collapse(session, cfg, &cgconf));

	if (!exists) {
		WT_ERR(__wt_metadata_insert(session, name, cgconf));
		WT_ERR(__wt_schema_open_colgroups(session, table));
	}

err:	__wt_free(session, cgconf);
	__wt_free(session, sourceconf);
	__wt_free(session, origconf);
	__wt_buf_free(session, &confbuf);
	__wt_buf_free(session, &fmt);
	__wt_buf_free(session, &namebuf);

	__wt_schema_release_table(session, table);
	return (ret);
}

/*
 * __wt_schema_index_source --
 *	Get the URI of the data source for an index.
 */
int
__wt_schema_index_source(WT_SESSION_IMPL *session,
    WT_TABLE *table, const char *idxname, const char *config, WT_ITEM *buf)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	size_t len;
	const char *prefix, *suffix, *tablename;

	tablename = table->name + strlen("table:");
	if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
	    !WT_STRING_MATCH("file", cval.str, cval.len)) {
		prefix = cval.str;
		len = cval.len;
		suffix = "_idx";
	} else {
		prefix = "file";
		len = strlen(prefix);
		suffix = ".wti";
	}
	WT_RET_NOTFOUND_OK(ret);

	WT_RET(__wt_buf_fmt(session, buf, "%.*s:%s_%s%s",
	    (int)len, prefix, tablename, idxname, suffix));

	return (0);
}

/*
 * __fill_index --
 *	Fill the index from the current contents of the table.
 */
static int
__fill_index(WT_SESSION_IMPL *session, WT_TABLE *table, WT_INDEX *idx)
{
	WT_DECL_RET;
	WT_CURSOR *tcur, *icur;
	WT_SESSION *wt_session;

	wt_session = &session->iface;
	tcur = NULL;
	icur = NULL;
	WT_RET(__wt_schema_open_colgroups(session, table));

	/*
	 * If the column groups have not been completely created,
	 * there cannot be data inserted yet, and we're done.
	 */
	if (!table->cg_complete)
		return (0);

	WT_ERR(wt_session->open_cursor(wt_session,
	    idx->source, NULL, "bulk=unordered", &icur));
	WT_ERR(wt_session->open_cursor(wt_session,
	    table->name, NULL, "readonly", &tcur));

	while ((ret = tcur->next(tcur)) == 0)
		WT_ERR(__wt_apply_single_idx(session, idx,
		    icur, (WT_CURSOR_TABLE *)tcur, icur->insert));

	WT_ERR_NOTFOUND_OK(ret);
err:
	if (icur)
		WT_TRET(icur->close(icur));
	if (tcur)
		WT_TRET(tcur->close(tcur));
	return (ret);
}

/*
 * __create_index --
 *	Create an index.
 */
static int
__create_index(WT_SESSION_IMPL *session,
    const char *name, bool exclusive, const char *config)
{
	WT_CONFIG kcols, pkcols;
	WT_CONFIG_ITEM ckey, cval, icols, kval;
	WT_DECL_PACK_VALUE(pv);
	WT_DECL_RET;
	WT_INDEX *idx;
	WT_ITEM confbuf, extra_cols, fmt, namebuf;
	WT_PACK pack;
	WT_TABLE *table;
	const char *cfg[4] =
	    { WT_CONFIG_BASE(session, index_meta), NULL, NULL, NULL };
	const char *sourcecfg[] = { config, NULL, NULL };
	const char *source, *sourceconf, *idxname, *tablename;
	char *idxconf, *origconf;
	size_t tlen;
	bool exists, have_extractor;
	u_int i, npublic_cols;

	sourceconf = NULL;
	idxconf = origconf = NULL;
	WT_CLEAR(confbuf);
	WT_CLEAR(fmt);
	WT_CLEAR(extra_cols);
	WT_CLEAR(namebuf);
	exists = have_extractor = false;

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "index:"))
		return (EINVAL);
	idxname = strchr(tablename, ':');
	if (idxname == NULL)
		WT_RET_MSG(session, EINVAL, "Invalid index name, "
		    "should be <table name>:<index name>: %s", name);

	tlen = (size_t)(idxname++ - tablename);
	if ((ret =
	    __wt_schema_get_table(session, tablename, tlen, true, &table)) != 0)
		WT_RET_MSG(session, ret,
		    "Can't create an index for a non-existent table: %.*s",
		    (int)tlen, tablename);

	if (table->is_simple)
		WT_ERR_MSG(session, EINVAL,
		    "%s requires a table with named columns", name);

	/* Check if the index already exists. */
	if ((ret = __wt_metadata_search(session, name, &origconf)) == 0) {
		if (exclusive)
			WT_ERR(EEXIST);
		exists = true;
	}
	WT_ERR_NOTFOUND_OK(ret);

	if (__wt_config_getones(session, config, "source", &cval) == 0) {
		WT_ERR(__wt_buf_fmt(session, &namebuf,
		    "%.*s", (int)cval.len, cval.str));
		source = namebuf.data;
	} else {
		WT_ERR(__wt_schema_index_source(
		    session, table, idxname, config, &namebuf));
		source = namebuf.data;

		/* Add the source name to the index config before collapsing. */
		WT_ERR(__wt_buf_catfmt(session, &confbuf,
		    ",source=\"%s\"", source));
	}

	if (__wt_config_getones_none(
	    session, config, "extractor", &cval) == 0 && cval.len != 0) {
		have_extractor = true;
		/* Custom extractors must supply a key format. */
		if ((ret = __wt_config_getones(
		    session, config, "key_format", &kval)) != 0)
			WT_ERR_MSG(session, EINVAL,
			    "%s: custom extractors require a key_format", name);
	}

	/* Calculate the key/value formats. */
	WT_CLEAR(icols);
	if (__wt_config_getones(session, config, "columns", &icols) != 0 &&
	    !have_extractor)
		WT_ERR_MSG(session, EINVAL,
		    "%s: requires 'columns' configuration", name);

	/*
	 * Count the public columns using the declared columns for normal
	 * indices or the key format for custom extractors.
	 */
	npublic_cols = 0;
	if (!have_extractor) {
		WT_ERR(__wt_config_subinit(session, &kcols, &icols));
		while ((ret = __wt_config_next(&kcols, &ckey, &cval)) == 0)
			++npublic_cols;
		WT_ERR_NOTFOUND_OK(ret);
	} else {
		WT_ERR(__pack_initn(session, &pack, kval.str, kval.len));
		while ((ret = __pack_next(&pack, &pv)) == 0)
			++npublic_cols;
		WT_ERR_NOTFOUND_OK(ret);
	}

	/*
	 * The key format for an index is somewhat subtle: the application
	 * specifies a set of columns that it will use for the key, but the
	 * engine usually adds some hidden columns in order to derive the
	 * primary key.  These hidden columns are part of the source's
	 * key_format, which we are calculating now, but not part of an index
	 * cursor's key_format.
	 */
	WT_ERR(__wt_config_subinit(session, &pkcols, &table->colconf));
	for (i = 0; i < table->nkey_columns &&
	    (ret = __wt_config_next(&pkcols, &ckey, &cval)) == 0;
	    i++) {
		/*
		 * If the primary key column is already in the secondary key,
		 * don't add it again.
		 */
		if (__wt_config_subgetraw(session, &icols, &ckey, &cval) == 0) {
			if (have_extractor)
				WT_ERR_MSG(session, EINVAL,
				    "an index with a custom extractor may not "
				    "include primary key columns");
			continue;
		}
		WT_ERR(__wt_buf_catfmt(
		    session, &extra_cols, "%.*s,", (int)ckey.len, ckey.str));
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Index values are empty: all columns are packed into the index key. */
	WT_ERR(__wt_buf_fmt(session, &fmt, "value_format=,key_format="));

	if (have_extractor) {
		WT_ERR(__wt_buf_catfmt(session, &fmt, "%.*s",
		    (int)kval.len, kval.str));
		WT_CLEAR(icols);
	}

	/*
	 * Construct the index key format, or append the primary key columns
	 * for custom extractors.
	 */
	WT_ERR(__wt_struct_reformat(session, table,
	    icols.str, icols.len, (const char *)extra_cols.data, false, &fmt));

	/* Check for a record number index key, which makes no sense. */
	WT_ERR(__wt_config_getones(session, fmt.data, "key_format", &cval));
	if (cval.len == 1 && cval.str[0] == 'r')
		WT_ERR_MSG(session, EINVAL,
		    "column-store index may not use the record number as its "
		    "index key");

	WT_ERR(__wt_buf_catfmt(
	    session, &fmt, ",index_key_columns=%u", npublic_cols));

	sourcecfg[1] = fmt.data;
	WT_ERR(__wt_config_merge(session, sourcecfg, NULL, &sourceconf));

	WT_ERR(__wt_schema_create(session, source, sourceconf));

	cfg[1] = sourceconf;
	cfg[2] = confbuf.data;
	WT_ERR(__wt_config_collapse(session, cfg, &idxconf));

	if (!exists) {
		WT_ERR(__wt_metadata_insert(session, name, idxconf));

		/* Make sure that the configuration is valid. */
		WT_ERR(__wt_schema_open_index(
		    session, table, idxname, strlen(idxname), &idx));

		/* If there is data in the table, fill the index. */
		WT_ERR(__fill_index(session, table, idx));
	}

err:	__wt_free(session, idxconf);
	__wt_free(session, origconf);
	__wt_free(session, sourceconf);
	__wt_buf_free(session, &confbuf);
	__wt_buf_free(session, &extra_cols);
	__wt_buf_free(session, &fmt);
	__wt_buf_free(session, &namebuf);

	__wt_schema_release_table(session, table);
	return (ret);
}

/*
 * __create_table --
 *	Create a table.
 */
static int
__create_table(WT_SESSION_IMPL *session,
    const char *name, bool exclusive, const char *config)
{
	WT_CONFIG conf;
	WT_CONFIG_ITEM cgkey, cgval, cval;
	WT_DECL_RET;
	WT_TABLE *table;
	const char *cfg[4] =
	    { WT_CONFIG_BASE(session, table_meta), config, NULL, NULL };
	const char *tablename;
	char *tableconf, *cgname;
	size_t cgsize;
	int ncolgroups;
	bool exists;

	cgname = NULL;
	table = NULL;
	tableconf = NULL;
	exists = false;

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "table:"))
		return (EINVAL);

	if ((ret = __wt_schema_get_table(session,
	    tablename, strlen(tablename), false, &table)) == 0) {
		if (exclusive)
			WT_ERR(EEXIST);
		exists = true;
	}
	WT_ERR_NOTFOUND_OK(ret);

	WT_ERR(__wt_config_gets(session, cfg, "colgroups", &cval));
	WT_ERR(__wt_config_subinit(session, &conf, &cval));
	for (ncolgroups = 0;
	    (ret = __wt_config_next(&conf, &cgkey, &cgval)) == 0;
	    ncolgroups++)
		;
	WT_ERR_NOTFOUND_OK(ret);

	WT_ERR(__wt_config_collapse(session, cfg, &tableconf));

	if (!exists) {
		WT_ERR(__wt_metadata_insert(session, name, tableconf));

		/* Attempt to open the table now to catch any errors. */
		WT_ERR(__wt_schema_get_table(
		    session, tablename, strlen(tablename), true, &table));

		if (ncolgroups == 0) {
			cgsize = strlen("colgroup:") + strlen(tablename) + 1;
			WT_ERR(__wt_calloc_def(session, cgsize, &cgname));
			snprintf(cgname, cgsize, "colgroup:%s", tablename);
			WT_ERR(__create_colgroup(
			    session, cgname, exclusive, config));
		}
	}

	if (0) {
err:		if (table != NULL) {
			WT_TRET(__wt_schema_remove_table(session, table));
			table = NULL;
		}
	}
	if (table != NULL)
		__wt_schema_release_table(session, table);
	__wt_free(session, cgname);
	__wt_free(session, tableconf);
	return (ret);
}

/*
 * __create_data_source --
 *	Create a custom data source.
 */
static int
__create_data_source(WT_SESSION_IMPL *session,
    const char *uri, const char *config, WT_DATA_SOURCE *dsrc)
{
	WT_CONFIG_ITEM cval;
	const char *cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_create), config, NULL };

	/*
	 * Check to be sure the key/value formats are legal: the underlying
	 * data source doesn't have access to the functions that check.
	 */
	WT_RET(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_RET(__wt_struct_confchk(session, &cval));
	WT_RET(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_RET(__wt_struct_confchk(session, &cval));

	/*
	 * User-specified collators aren't supported for data-source objects.
	 */
	if (__wt_config_getones_none(
	    session, config, "collator", &cval) != WT_NOTFOUND && cval.len != 0)
		WT_RET_MSG(session, EINVAL,
		    "WT_DATA_SOURCE objects do not support WT_COLLATOR "
		    "ordering");

	return (dsrc->create(dsrc, &session->iface, uri, (WT_CONFIG_ARG *)cfg));
}

/*
 * __wt_schema_create --
 *	Process a WT_SESSION::create operation for all supported types.
 */
int
__wt_schema_create(
    WT_SESSION_IMPL *session, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;
	bool exclusive;

	exclusive =
	    __wt_config_getones(session, config, "exclusive", &cval) == 0 &&
	    cval.val != 0;

	/*
	 * We track create operations: if we fail in the middle of creating a
	 * complex object, we want to back it all out.
	 */
	WT_RET(__wt_meta_track_on(session));

	if (WT_PREFIX_MATCH(uri, "colgroup:"))
		ret = __create_colgroup(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __create_file(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "lsm:"))
		ret = __wt_lsm_tree_create(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __create_index(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __create_table(session, uri, exclusive, config);
	else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
		ret = dsrc->create == NULL ?
		    __wt_object_unsupported(session, uri) :
		    __create_data_source(session, uri, config, dsrc);
	else
		ret = __wt_bad_object_type(session, uri);

	session->dhandle = NULL;
	WT_TRET(__wt_meta_track_off(session, true, ret != 0));

	return (ret);
}
