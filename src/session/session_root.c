/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __snap_get(WT_SESSION_IMPL *, const char *, WT_ITEM *);
static int __snap_get_last(WT_SESSION_IMPL *, const char *, WT_ITEM *);
static int __snap_get_name(
	WT_SESSION_IMPL *, const char *, const char *, WT_ITEM *);
static int __snap_get_turtle(WT_SESSION_IMPL *, WT_ITEM *);
static int __snap_get_turtle_config(WT_SESSION_IMPL *, char *, size_t);
static int __snap_set(WT_SESSION_IMPL *, const char *, const char *);
static int __snap_set_turtle(WT_SESSION_IMPL *, const char *);
static int __snap_version_chk(WT_SESSION_IMPL *, const char *);

/*
 * __wt_session_snap_get --
 *	Get the file's most recent snapshot address.
 */
int
__wt_session_snap_get(WT_SESSION_IMPL *session, const char *name, WT_ITEM *addr)
{
	WT_BTREE *btree;
	int ret;

	btree = session->btree;
	ret = 0;

	/* Get the snapshot address. */
	ret = strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0 ?
	    __snap_get_turtle(session, addr) : __snap_get(session, name, addr);

	/*
	 * If we find a snapshot, check the version and return the address.
	 * If we don't find a named snapshot, we're done, they're read-only.
	 * If we don't find a default snapshot, it's creation, return "no
	 * data" and let our caller handle it.
	 */
	if (ret == WT_NOTFOUND) {
		/*
		 * If the caller didn't give us a specific snapshot name, we
		 * assume it's a creation and there isn't a snapshot to find.
		 * Let the caller deal with the failure.
		 */
		if (name != NULL)
			WT_RET_MSG(session, WT_NOTFOUND,
			    "unable to find a \"%s\" snapshot entry for the "
			    "file %s",
			    name, btree->filename);

		addr->data = NULL;
		addr->size = 0;
	}
	return (0);
}

/*
 * __wt_session_snap_clear --
 *	Clear a file's snapshot information.
 */
int
__wt_session_snap_clear(WT_SESSION_IMPL *session, const char *filename)
{
	int ret;

	ret = strcmp(filename, WT_SCHEMA_FILENAME) == 0 ?
	    __snap_set_turtle(session, NULL) :
	    __snap_set(session, filename, NULL);

	/*
	 * If we are unrolling a failed create, we may have already removed the
	 * schema table entry.  If no entry is found to update and we're trying
	 * to clear the snapshot, just ignore it.
	 */
	if (ret == WT_NOTFOUND)
		ret = 0;
	return (ret);
}

/*
 * __snap_get_turtle --
 *	Get the schema file's snapshot address.
 */
static int
__snap_get_turtle(
    WT_SESSION_IMPL *session, WT_ITEM *addr)
{
	char line[1024];

	WT_RET(__snap_get_turtle_config(session, line, sizeof(line)));

	/* Check the major/minor version numbers. */
	WT_RET(__snap_version_chk(session, line));

	/* Retrieve the last snapshot (there should only be one). */
	WT_RET(__snap_get_last(session, line, addr));

	return (0);
}

/*
 * __snap_get_turtle_config --
 *	Return the configuration value for the schema-table.
 */
static int
__snap_get_turtle_config(
    WT_SESSION_IMPL *session, char *line, size_t len)
{
	const char *path;
	FILE *fp;
	int ret;
	char *p;

	fp = NULL;
	ret = 0;
	path = NULL;

	/* Retrieve the turtle file's entry. */
	WT_RET(__wt_filename(session, WT_SCHEMA_TURTLE, &path));
	WT_ERR_TEST((fp = fopen(path, "r")) == NULL, WT_NOTFOUND);
	while (fgets(line, (int)len, fp) != NULL) {
		if ((p = strchr(line, '\n')) == NULL)
			break;
		*p = '\0';
		if (strcmp(line, WT_SCHEMA_TURTLE_MSG) == 0)
			continue;
	}
	if (ferror(fp))
		ret = __wt_errno();
	if (fp != NULL)
		WT_TRET(fclose(fp));

	if (ret != 0)
		__wt_errx(session, "the %s file is corrupted", path);

err:	__wt_free(session, path);
	return (ret);
}

/*
 * __snap_set_turtle --
 *	Set the schema file's snapshot address.
 */
static int
__snap_set_turtle(WT_SESSION_IMPL *session, const char *v)
{
	WT_ITEM *buf;
	FILE *fp;
	size_t len;
	int ret;
	const char *path;

	buf = NULL;
	ret = 0;
	path = NULL;

	WT_ERR(__wt_filename(session, WT_SCHEMA_TURTLE_SET, &path));
	WT_ERR_TEST((fp = fopen(path, "w")) == NULL, __wt_errno());

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "%s\n"
	    "version=(major=%d,minor=%d),"
	    "%s\n",
	    WT_SCHEMA_TURTLE_MSG,
	    WT_BTREE_MAJOR_VERSION, WT_BTREE_MINOR_VERSION,
	    v == NULL ? "snapshot=()" : v));
	len = (size_t)fprintf(fp, "%s", (char *)buf->data);
	if (len != buf->size)
		ret = WT_ERROR;

	WT_TRET(fflush(fp));
	WT_TRET(fclose(fp));

	if (ret == 0)
		ret = __wt_rename(
		    session, WT_SCHEMA_TURTLE_SET, WT_SCHEMA_TURTLE);
	else
		(void)__wt_remove(session, WT_SCHEMA_TURTLE_SET);

err:	if (path != NULL)
		__wt_free(session, path);
	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __snap_get --
 *	Return the version and snapshot information for a schema-table entry.
 */
static int
__snap_get(WT_SESSION_IMPL *session, const char *name, WT_ITEM *addr)
{
	WT_BTREE *btree;
	WT_ITEM *key;
	const char *config;
	int ret;

	btree = session->btree;
	config = NULL;
	key = NULL;
	ret = 0;

	/* Retrieve the schema-table's entry for the file. */
	WT_ERR(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "file:%s", btree->filename));
	WT_ERR(__wt_schema_table_read(session, key->data, &config));

	/* Check the major/minor version numbers. */
	WT_ERR(__snap_version_chk(session, config));

	/* Retrieve the named snapshot or the last snapshot. */
	if (name == NULL)
		WT_ERR(__snap_get_last(session, config, addr));
	else
		WT_ERR(__snap_get_name(session, name, config, addr));

err:	__wt_scr_free(&key);
	__wt_free(session, config);
	return (ret);
}

/*
 * __snap_set --
 *	Set a non-schema file's snapshot address.
 */
static int
__snap_set(WT_SESSION_IMPL *session, const char *filename, const char *v)
{
	WT_ITEM *key;
	const char *config, *cfg[3], *newcfg;
	int ret;

	key = NULL;
	config = newcfg = NULL;
	ret = 0;

	/* Retrieve the schema table information for this file. */
	WT_ERR(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "file:%s", filename));
	WT_ERR(__wt_schema_table_read(session, key->data, &config));

	/* Replace the snapshot entry. */
	cfg[0] = config;
	cfg[1] = v == NULL ? "snapshot=()" : v;
	cfg[2] = NULL;
	WT_ERR(__wt_config_collapse(session, cfg, &newcfg));
	WT_ERR(__wt_schema_table_update(session, key->data, newcfg));

err:	__wt_scr_free(&key);
	__wt_free(session, config);
	__wt_free(session, newcfg);
	return (ret);
}

/*
 * __snap_get_name --
 *	Return the cookie associated with a single snapshot in a schema-table
 * entry.
 */
static int
__snap_get_name(WT_SESSION_IMPL *session,
    const char *name, const char *config, WT_ITEM *addr)
{
	WT_CONFIG snapconf;
	WT_CONFIG_ITEM a, k, v;

	WT_RET(__wt_config_getones(session, config, "snapshot", &v));
	WT_RET(__wt_config_subinit(session, &snapconf, &v));
	while (__wt_config_next(&snapconf, &k, &v) == 0)
		if (strlen(name) == k.len && strncmp(name, k.str, k.len) == 0) {
			WT_RET(__wt_config_subgets(session, &v, "addr", &a));
			WT_RET(__wt_nhex_to_raw(session, a.str, a.len, addr));
			return (0);
		}
	return (WT_NOTFOUND);
}

/*
 * __snap_get_last --
 *	Return the cookie associated with the last snapshot in a schema-table
 * entry.
 */
static int
__snap_get_last(
    WT_SESSION_IMPL *session, const char *config, WT_ITEM *addr)
{
	WT_CONFIG snapconf;
	WT_CONFIG_ITEM a, k, v;
	int64_t found;

	WT_RET(__wt_config_getones(session, config, "snapshot", &v));
	WT_RET(__wt_config_subinit(session, &snapconf, &v));
	for (found = 0; __wt_config_next(&snapconf, &k, &v) == 0;) {
		if (found) {
			WT_RET(__wt_config_subgets(session, &v, "order", &a));
			if (a.val < found)
				continue;
		}

		WT_RET(__wt_config_subgets(session, &v, "addr", &a));
		if (a.len == 0)
			WT_RET(EINVAL);

		/* Our caller wants the raw cookie, not the hex. */
		WT_RET(__wt_nhex_to_raw(session, a.str, a.len, addr));
		WT_RET(__wt_config_subgets(session, &v, "order", &a));
		found = a.val;
	}

	return (found ? 0 : WT_NOTFOUND);
}

/*
 * __snap_compare_order --
 *	Qsort comparison routine for the snapshot list.
 */
static int
__snap_compare_order(const void *a, const void *b)
{
	WT_SNAPSHOT *asnap, *bsnap;

	asnap = (WT_SNAPSHOT *)a;
	bsnap = (WT_SNAPSHOT *)b;

	return (asnap->order > bsnap->order ? 1 : -1);
}

/*
 * __wt_snap_list_get, __wt_session_snap_list_get --
 *	Load all available snapshot information from a schema-table entry.
 */
int
__wt_snap_list_get(
    WT_SESSION *session, const char *config, WT_SNAPSHOT **snapbasep)
{
	WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;

	return (__wt_session_snap_list_get(session_impl, config, snapbasep));
}
int
__wt_session_snap_list_get(
    WT_SESSION_IMPL *session, const char *config_arg, WT_SNAPSHOT **snapbasep)
{
	WT_BTREE *btree;
	WT_CONFIG snapconf;
	WT_CONFIG_ITEM a, k, v;
	WT_ITEM *key;
	WT_SNAPSHOT *snap, *snapbase;
	size_t allocated, slot;
	int ret;
	const char *config;
	char line[1024], timebuf[64];

	*snapbasep = NULL;

	btree = session->btree;
	key = NULL;
	snapbase = NULL;
	allocated = slot = 0;
	config = NULL;

	/*
	 * If no configuration information was provided, retrieve the schema
	 * table information for the current file or the configuration line
	 * for the schema table itself.
	 */
	if (config_arg != NULL)
		config = config_arg;
	else if (strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0) {
		line[0] = '\0';
		ret = __snap_get_turtle_config(session, line, sizeof(line));
		if (ret != 0 && ret != WT_NOTFOUND)
			return (ret);
		config = line;
	} else {
		WT_ERR(__wt_scr_alloc(session, 0, &key));
		WT_ERR(__wt_buf_fmt(session, key, "file:%s", btree->filename));
		WT_ERR(__wt_schema_table_read(session, key->data, &config));
	}

	/* Load any existing snapshots into the array. */
	if (__wt_config_getones(session, config, "snapshot", &v) == 0 &&
	    __wt_config_subinit(session, &snapconf, &v) == 0)
		for (; __wt_config_next(&snapconf, &k, &v) == 0; ++slot) {
			if (slot * sizeof(WT_SNAPSHOT) == allocated)
				WT_ERR(__wt_realloc(session, &allocated,
				    (slot + 50) * sizeof(WT_SNAPSHOT),
				    &snapbase));
			snap = &snapbase[slot];

			/*
			 * Copy the name, address (raw and hex), order and time
			 * into the slot.
			 */
			WT_ERR(
			    __wt_strndup(session, k.str, k.len, &snap->name));

			WT_ERR(__wt_config_subgets(session, &v, "addr", &a));
			if (a.len == 0)
				goto format;
			WT_ERR(__wt_buf_set(
			    session, &snap->addr, a.str, a.len));
			WT_ERR(__wt_nhex_to_raw(
			    session, a.str, a.len, &snap->raw));

			WT_ERR(__wt_config_subgets(session, &v, "order", &a));
			if (a.val == 0)
				goto format;
			snap->order = a.val;

			WT_ERR(__wt_config_subgets(session, &v, "time", &a));
			if (a.len == 0)
				goto format;
			if (a.len > sizeof(timebuf) - 1)
				goto format;
			memcpy(timebuf, a.str, a.len);
			timebuf[a.len] = '\0';
			if (sscanf(timebuf,
			    "%" SCNu64 ".%" SCNu64,
			    &snap->sec, &snap->nsec) != 2)
				goto format;

			WT_ERR(__wt_config_subgets(session, &v, "size", &a));
			snap->snapshot_size = a.val;
		}

	/*
	 * Allocate an extra slot for a new value, plus a slot to mark the end.
	 *
	 * This isn't very clean, but there's necessary cooperation between the
	 * schema layer (that maintains the list of snapshots), the btree layer
	 * (that knows when the root page is written, creating a new snapshot),
	 * and the block manager (which actually creates the snapshot).  All of
	 * that cooperation is handled in the WT_SNAPSHOT structure referenced
	 * from the WT_BTREE structure.
	 */
	if ((slot + 2) * sizeof(WT_SNAPSHOT) >= allocated)
		WT_ERR(__wt_realloc(session, &allocated,
		    (slot + 2) * sizeof(WT_SNAPSHOT), &snapbase));

	/* Sort in creation-order. */
	qsort(snapbase, slot, sizeof(WT_SNAPSHOT), __snap_compare_order);

	/* Return the array to our caller. */
	*snapbasep = snapbase;

	if (0) {
format:		WT_ERR_MSG(session, WT_ERROR,
		    "%s: corrupted snapshot list", btree->filename);
err:		__wt_session_snap_list_free(session, snapbase);
	}
	if (config != config_arg && config != line)
		__wt_free(session, config);
	__wt_scr_free(&key);

	return (ret);
}

/*
 * __wt_session_snap_list_set --
 *	Set a schema-table entry snapshot from the WT_SNAPSHOT list.
 */
int
__wt_session_snap_list_set(WT_SESSION_IMPL *session, WT_SNAPSHOT *snapbase)
{
	WT_BTREE *btree;
	WT_ITEM *buf;
	WT_SNAPSHOT *snap;
	int64_t order;
	int ret;
	const char *sep;

	btree = session->btree;
	buf = NULL;
	ret = 0;

	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	order = 0;
	sep = "";
	WT_ERR(__wt_buf_fmt(session, buf, "snapshot=("));
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		/* Skip deleted snapshots. */
		if (FLD_ISSET(snap->flags, WT_SNAP_DELETE))
			continue;

		/*
		 * Track the largest active snapshot counter: it's not really
		 * a generational number or an ID because we reset it to 1 if
		 * the snapshot we're writing is the only snapshot the file has.
		 * The problem we're solving is when two snapshots are taken
		 * quickly, the timer may not be unique and/or we can even see
		 * time travel on the second snapshot if we read the time
		 * in-between nanoseconds rolling over.  All we need to know
		 * is the real snapshot order so we don't accidentally take the
		 * wrong "last" snapshot.
		 */
		if (snap->order > order)
			order = snap->order;

		if (FLD_ISSET(snap->flags, WT_SNAP_ADD | WT_SNAP_UPDATE)) {
			/* Convert the raw cookie to a hex string. */
			WT_ERR(__wt_raw_to_hex(session,
			    snap->raw.data, snap->raw.size, &snap->addr));

			if (FLD_ISSET(snap->flags, WT_SNAP_ADD))
				snap->order = order + 1;
		}
		WT_ERR(__wt_buf_catfmt(session, buf,
		    "%s%s=(addr=\"%.*s\",order=%" PRIu64
		    ",time=%" PRIu64 ".%" PRIu64 ",size=%" PRIu64 ")",
		    sep, snap->name,
		    (int)snap->addr.size, (char *)snap->addr.data,
		    snap->order,
		    snap->sec, snap->nsec, snap->snapshot_size));
		sep = ",";
	}
	WT_ERR(__wt_buf_catfmt(session, buf, ")"));
	WT_ERR(strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0 ?
	    __snap_set_turtle(session, (char *)buf->data) :
	    __snap_set(session, btree->filename, (char *)buf->data));

err:	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __wt_snap_list_free, __wt_session_snap_list_free --
 *	Discard the snapshot array.
 */
void
__wt_snap_list_free(WT_SESSION *session, WT_SNAPSHOT *snapbase)
{
	WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;

	return (__wt_session_snap_list_free(session_impl, snapbase));
}
void
__wt_session_snap_list_free(WT_SESSION_IMPL *session, WT_SNAPSHOT *snapbase)
{
	WT_SNAPSHOT *snap;
	if (snapbase == NULL)
		return;
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		__wt_free(session, snap->name);
		__wt_buf_free(session, &snap->addr);
		__wt_buf_free(session, &snap->raw);
		__wt_free(session, snap->bpriv);
	}
	__wt_free(session, snapbase);
}

/*
 * __snap_version_chk --
 *	Check the version major/minor numbers.
 */
static int
__snap_version_chk(WT_SESSION_IMPL *session, const char *config)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM a, v;
	int majorv, minorv;

	btree = session->btree;

	WT_RET(__wt_config_getones(session, config, "version", &v));
	WT_RET(__wt_config_subgets(session, &v, "major", &a));
	majorv = (int)a.val;
	WT_RET(__wt_config_subgets(session, &v, "minor", &a));
	minorv = (int)a.val;

	if (majorv > WT_BTREE_MAJOR_VERSION ||
	    (majorv == WT_BTREE_MAJOR_VERSION &&
	    minorv > WT_BTREE_MINOR_VERSION))
		WT_RET_MSG(session, EACCES,
		    "%s is an unsupported version of a WiredTiger file",
		    btree->filename);
	return (0);
}
