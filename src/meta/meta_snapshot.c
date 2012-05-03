/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __snap_get(
	WT_SESSION_IMPL *, const char *, const char *,  WT_ITEM *);
static int __snap_get_last(WT_SESSION_IMPL *, const char *, WT_ITEM *);
static int __snap_get_name(
	WT_SESSION_IMPL *, const char *, const char *, WT_ITEM *);
static int __snap_set(WT_SESSION_IMPL *, const char *, const char *);
static int __snap_version_chk(WT_SESSION_IMPL *, const char *, const char *);

/*
 * __wt_snapshot_get --
 *	Get the file's most recent snapshot address.
 */
int
__wt_snapshot_get(WT_SESSION_IMPL *session,
    const char *name, const char *snapshot, WT_ITEM *addr)
{
	WT_DECL_RET;

	/* Get the snapshot address. */
	ret = __snap_get(session, name, snapshot, addr);

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
		if (snapshot != NULL)
			WT_RET_MSG(session, WT_NOTFOUND,
			    "no \"%s\" snapshot found in %s",
			    snapshot, name);

		addr->data = NULL;
		addr->size = 0;
	}
	return (0);
}

/*
 * __wt_snapshot_clear --
 *	Clear a file's snapshot.
 */
int
__wt_snapshot_clear(WT_SESSION_IMPL *session, const char *name)
{
	WT_DECL_RET;

	ret = __snap_set(session, name, NULL);

	/*
	 * If we are unrolling a failed create, we may have already removed the
	 * metadata entry.  If no entry is found to update and we're trying to
	 * clear the snapshot, just ignore it.
	 */
	if (ret == WT_NOTFOUND)
		ret = 0;
	return (ret);
}

/*
 * __snap_get --
 *	Get a file's snapshot.
 */
static int
__snap_get(WT_SESSION_IMPL *session,
    const char *name, const char *snapshot, WT_ITEM *addr)
{
	WT_DECL_RET;
	const char *config;

	config = NULL;

	/* Retrieve the metadata entry for the file. */
	WT_ERR(__wt_metadata_read(session, name, &config));

	/* Check the major/minor version numbers. */
	WT_ERR(__snap_version_chk(session, name, config));

	/* Retrieve the named snapshot or the last snapshot. */
	if (snapshot == NULL)
		WT_ERR(__snap_get_last(session, config, addr));
	else
		WT_ERR(__snap_get_name(session, snapshot, config, addr));

err:	__wt_free(session, config);
	return (ret);
}

/*
 * __snap_set --
 *	Set a file's snapshot.
 */
static int
__snap_set(WT_SESSION_IMPL *session, const char *name, const char *v)
{
	WT_DECL_RET;
	const char *config, *cfg[3], *newcfg;

	config = newcfg = NULL;

	/* Retrieve the metadata for this file. */
	WT_ERR(__wt_metadata_read(session, name, &config));

	/* Replace the snapshot entry. */
	cfg[0] = config;
	cfg[1] = v == NULL ? "snapshot=()" : v;
	cfg[2] = NULL;
	WT_ERR(__wt_config_collapse(session, cfg, &newcfg));
	WT_ERR(__wt_metadata_update(session, name, newcfg));

err:	__wt_free(session, config);
	__wt_free(session, newcfg);
	return (ret);
}

/*
 * __snap_get_name --
 *	Return the cookie associated with a file's named snapshot.
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
 *	Return the cookie associated with the file's last snapshot.
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
 * __wt_snapshot_list_get --
 *	Load all available snapshot information for a file.
 */
int
__wt_snapshot_list_get(
    WT_SESSION_IMPL *session, const char *name, WT_SNAPSHOT **snapbasep)
{
	WT_CONFIG snapconf;
	WT_CONFIG_ITEM a, k, v;
	WT_DECL_RET;
	WT_ITEM *buf;
	WT_SNAPSHOT *snap, *snapbase;
	size_t allocated, slot;
	const char *config;
	char timebuf[64];

	*snapbasep = NULL;

	buf = NULL;
	snapbase = NULL;
	allocated = slot = 0;
	config = NULL;

	/* Retrieve the metadata information for the file. */
	WT_RET(__wt_metadata_read(session, name, &config));

	/* Load any existing snapshots into the array. */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
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
			if (sscanf(timebuf, "%" SCNuMAX, &snap->sec) != 1)
				goto format;

			WT_ERR(__wt_config_subgets(session, &v, "size", &a));
			snap->snapshot_size = (uint64_t)a.val;
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
format:		WT_ERR_MSG(session, WT_ERROR, "corrupted snapshot list");
err:		__wt_snapshot_list_free(session, snapbase);
	}
	__wt_free(session, config);
	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __wt_snapshot_list_set --
 *	Set a file's snapshot value from the WT_SNAPSHOT list.
 */
int
__wt_snapshot_list_set(
    WT_SESSION_IMPL *session, const char *name, WT_SNAPSHOT *snapbase)
{
	WT_DECL_RET;
	WT_ITEM *buf;
	WT_SNAPSHOT *snap;
	int64_t order;
	const char *sep;

	buf = NULL;

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
		    ",time=%" PRIuMAX ",size=%" PRIu64 ")",
		    sep, snap->name,
		    (int)snap->addr.size, (char *)snap->addr.data,
		    snap->order, snap->sec, snap->snapshot_size));
		sep = ",";
	}
	WT_ERR(__wt_buf_catfmt(session, buf, ")"));
	WT_ERR(__snap_set(session, name, buf->mem));

err:	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __wt_snapshot_list_free --
 *	Discard the snapshot array.
 */
void
__wt_snapshot_list_free(WT_SESSION_IMPL *session, WT_SNAPSHOT *snapbase)
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
__snap_version_chk(
    WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_CONFIG_ITEM a, v;
	int majorv, minorv;

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
		    name);
	return (0);
}
