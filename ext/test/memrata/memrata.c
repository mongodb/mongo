/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kvs.h>
#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#undef	INLINE
#define	INLINE	inline				/* Turn off inline */

/*
 * Macros to output an error message and set or return an error.
 * Requires local variables:
 *	int ret;
 */
#undef	ERET
#define	ERET(session, v, ...) do {					\
	(void)wt_ext->err_printf(					\
	    wt_ext, session, "memrata: " __VA_ARGS__);			\
	return (v);							\
} while (0)
#undef	ESET
#define	ESET(session, v, ...) do {					\
	(void)wt_ext->err_printf(					\
	    wt_ext, session, "memrata: " __VA_ARGS__);			\
	if (ret == 0)							\
		ret = v;						\
} while (0)
#undef	ETRET
#define	ETRET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0 &&					\
	    (__ret == WT_PANIC ||					\
	    ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND))	\
		ret = __ret;						\
} while (0)

typedef struct __data_source {
	kvs_t kvs;				/* Underlying KVS store */

	char  *uri;				/* Object URI */
	pthread_rwlock_t lock;			/* Object lock */

	uint64_t append_recno;			/* Allocation record number */

	u_int	open_cursors;			/* Object's cursor count */

	struct __data_source *next;		/* List of objects */
} DATA_SOURCE;

typedef struct __cursor {
	WT_CURSOR wt_cursor;			/* Must come first */

	WT_SESSION *session;			/* Enclosing session */

	DATA_SOURCE *data_source;		/* Underlying  KVS object */

	struct kvs_record record;		/* Record */
	char   key[KVS_MAX_KEY_LEN];		/* key, value */
	char  *val;
	size_t val_len;

	int	 config_append;			/* config "append" */
	int	 config_bitfield;		/* config "value_format=#t" */
	int	 config_overwrite;		/* config "overwrite" */
	int	 config_recno;			/* config "key_format=r" */
} CURSOR;

static WT_EXTENSION_API *wt_ext;		/* Extension functions */

static DATA_SOURCE *data_source_head;		/* List of objects */
static pthread_rwlock_t global_lock;		/* Object list lock */

/*
 * os_errno --
 *	Limit our use of errno so it's easy to remove.
 */
static int
os_errno()
{
	return (errno);
}

/*
 * lock_init --
 *	Initialize an object's lock.
 */
static int
lock_init(WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret;

	if ((ret = pthread_rwlock_init(lockp, NULL)) != 0)
		ERET(session, WT_PANIC, "lock init: %s", strerror(ret));
	return (ret);
}

/*
 * lock_destroy --
 *	Destroy an object's lock.
 */
static int
lock_destroy(WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret;

	if ((ret = pthread_rwlock_destroy(lockp)) != 0)
		ERET(session, WT_PANIC, "lock destroy: %s", strerror(ret));
	return (0);
}

/*
 * writelock --
 *	Acquire a write lock.
 */
static INLINE int
writelock(WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret;

	if ((ret = pthread_rwlock_wrlock(lockp)) != 0)
		ERET(session, WT_PANIC, "lock: %s", strerror(ret));
	return (ret);
}

/*
 * unlock --
 *	Release an object's lock.
 */
static INLINE int
unlock(WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret;

	if ((ret = pthread_rwlock_unlock(lockp)) != 0)
		ERET(session, WT_PANIC, "unlock: %s", strerror(ret));
	return (0);
}

/*
 * copyin_key --
 *	Connect a WT_CURSOR key to a struct kvs_record key.
 */
static INLINE int
copyin_key(WT_CURSOR *wt_cursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_SESSION *session;
	size_t size;
	int ret;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	r = &cursor->record;
	ret = 0;

	/*
	 * XXX
	 * The underlying KVS library data fields aren't const.
	 */
	if (cursor->config_recno) {
		if ((ret = wt_ext->struct_size(wt_ext, session,
		    &size, "r", wt_cursor->recno)) != 0 ||
		    (ret = wt_ext->struct_pack(wt_ext, session,
		    r->key, sizeof(r->key), "r", wt_cursor->recno)) != 0)
			return (ret);
		r->key_len = size;
	} else {
		r->key = (char *)wt_cursor->key.data;
		r->key_len = (unsigned long)wt_cursor->key.size;
	}
	return (0);
}

/*
 * copyout_key --
 *	Connect a struct kvs_record key to a WT_CURSOR key.
 */
static INLINE int
copyout_key(WT_CURSOR *wt_cursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	r = &cursor->record;
	ret = 0;

	if (cursor->config_recno) {
		if ((ret = wt_ext->struct_unpack(wt_ext, session,
		    r->key, sizeof(r->key), "r", &wt_cursor->recno)) != 0)
			return (ret);
	} else {
		wt_cursor->key.data = r->key;
		wt_cursor->key.size = (uint32_t)r->key_len;
	}
	return (0);
}

/*
 * copyin_val --
 *	Connect a WT_CURSOR value to a struct kvs_record value .
 */
static INLINE int
copyin_val(WT_CURSOR *wt_cursor)
{
	struct kvs_record *r;
	CURSOR *cursor;

	cursor = (CURSOR *)wt_cursor;
	r = &cursor->record;

	/*
	 * XXX
	 * The underlying KVS library data fields aren't const.
	 */
	r->val = (char *)wt_cursor->value.data;
	r->val_len = (unsigned long)wt_cursor->value.size;
	return (0);
}

/*
 * copyout_val --
 *	Connect a struct kvs_record value to a WT_CURSOR value.
 */
static INLINE int
copyout_val(WT_CURSOR *wt_cursor)
{
	struct kvs_record *r;
	CURSOR *cursor;

	cursor = (CURSOR *)wt_cursor;
	r = &cursor->record;

	wt_cursor->value.data = r->val;
	wt_cursor->value.size = (uint32_t)r->val_len;
	return (0);
}

/*
 * copy_key --
 *	Copy the key for methods where the underlying KVS call returns a key.
 */
static INLINE int
copy_key(WT_CURSOR *wt_cursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_SESSION *session;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	r = &cursor->record;

	if (cursor->key == r->key)
		return (0);
	if (r->key_len > sizeof(cursor->key))
		ERET(session, ERANGE,
		    "key too large, maximum is %" PRIuMAX,
		    (uintmax_t)sizeof(cursor->key));
	memcpy(cursor->key, r->key, r->key_len);
	r->key = cursor->key;
	return (0);
}

/*
 * kvs_call --
 *	Call a KVS function.
 */
static INLINE int
kvs_call(WT_CURSOR *wt_cursor, const char *fname,
    int (*f)(kvs_t, struct kvs_record *, unsigned long, unsigned long))
{
	CURSOR *cursor;
	WT_SESSION *session;
	kvs_t kvs;
	int ret;
	char *p;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	kvs = cursor->data_source->kvs;
	ret = 0;

	cursor->record.val = cursor->val;

restart:
	if ((ret = f(kvs, &cursor->record,
	    (unsigned long)0, (unsigned long)cursor->val_len)) != 0) {
		if (ret == KVS_E_KEY_NOT_FOUND)
			return (WT_NOTFOUND);
		ERET(session, WT_ERROR, "%s: %s", fname, kvs_strerror(ret));
	}

	/*
	 * If the returned length is larger than our passed-in length, we didn't
	 * get the complete value.  Grow the buffer and use kvs_get to complete
	 * the retrieval (kvs_get because the call succeeded and the key was
	 * copied out, so calling kvs_next/kvs_prev again would skip key/value
	 * pairs).
	 *
	 * We have to loop, another thread of control might change the length of
	 * the value, requiring we grow our buffer multiple times.
	 *
	 * We have to potentially restart the entire call in case the underlying
	 * key/value disappears.
	 */
	for (;;) {
		if (cursor->val_len >= cursor->record.val_len)
			return (0);

		/* Grow the value buffer. */
		if ((p = realloc(
		    cursor->val, cursor->record.val_len + 32)) == NULL)
			return (os_errno());
		cursor->val = cursor->record.val = p;
		cursor->val_len = cursor->record.val_len + 32;

		if ((ret = kvs_get(kvs, &cursor->record,
		    (unsigned long)0, (unsigned long)cursor->val_len)) != 0) {
			if (ret == KVS_E_KEY_NOT_FOUND)
				goto restart;
			ERET(session,
			    WT_ERROR, "kvs_get: %s", kvs_strerror(ret));
		}
	}
	/* NOTREACHED */
}

/*
 * kvs_cursor_next --
 *	WT_CURSOR::next method.
 */
static int
kvs_cursor_next(WT_CURSOR *wt_cursor)
{
	int ret;

	if ((ret = copy_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = kvs_call(wt_cursor, "kvs_next", kvs_next)) != 0)
		return (ret);
	if ((ret = copyout_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = copyout_val(wt_cursor)) != 0)
		return (ret);
	return (0);
}

/*
 * kvs_cursor_prev --
 *	WT_CURSOR::prev method.
 */
static int
kvs_cursor_prev(WT_CURSOR *wt_cursor)
{
	int ret;

	if ((ret = copy_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = kvs_call(wt_cursor, "kvs_prev", kvs_prev)) != 0)
		return (ret);
	if ((ret = copyout_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = copyout_val(wt_cursor)) != 0)
		return (ret);
	return (0);
}

/*
 * kvs_cursor_reset --
 *	WT_CURSOR::reset method.
 */
static int
kvs_cursor_reset(WT_CURSOR *wt_cursor)
{
	CURSOR *cursor;

	cursor = (CURSOR *)wt_cursor;

	/*
	 * Reset the cursor by setting the key length to 0, causing subsequent
	 * next/prev operations to return the first/last record of the object.
	 */
	cursor->record.key_len = 0;
	return (0);
}

/*
 * kvs_cursor_search --
 *	WT_CURSOR::search method.
 */
static int
kvs_cursor_search(WT_CURSOR *wt_cursor)
{
	int ret;

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = kvs_call(wt_cursor, "kvs_get", kvs_get)) != 0)
		return (ret);
	if ((ret = copyout_val(wt_cursor)) != 0)
		return (ret);
	return (0);
}

/*
 * kvs_cursor_search_near --
 *	WT_CURSOR::search_near method.
 */
static int
kvs_cursor_search_near(WT_CURSOR *wt_cursor, int *exact)
{
	int ret;

	/*
	 * XXX
	 * I'm not confident this is sufficient: if there are multiple threads
	 * of control, it's possible for the search for an exact match to fail,
	 * another thread of control to insert (and commit) an exact match, and
	 * then it's possible we'll return the wrong value.  This needs to be
	 * revisited once the transactional code is in place.
	 */

	/* Search for an exact match. */
	if ((ret = kvs_cursor_search(wt_cursor)) == 0) {
		*exact = 0;
		return (0);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	/* Search for a key that's larger. */
	if ((ret = kvs_cursor_next(wt_cursor)) == 0) {
		*exact = 1;
		return (0);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	/* Search for a key that's smaller. */
	if ((ret = kvs_cursor_prev(wt_cursor)) == 0) {
		*exact = -1;
		return (0);
	}

	return (ret);
}

/*
 * kvs_recno_alloc --
 *	Allocate a new record number.
 */
static INLINE int
kvs_recno_alloc(WT_CURSOR *wt_cursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	r = &cursor->record;

	/* Lock the data-source. */
	if ((ret = writelock(session, &cursor->data_source->lock)) != 0)
		return (ret);

	/*
	 * If not yet tracking the maximum record number, read the last record
	 * from the object and figure it out.
	 *
	 * XXX
	 * There is still a race here.  If another thread of control performs a
	 * WT_CURSOR::insert of an explicit record number (that is, the other
	 * cursor isn't configured for "append"), we could race.  If we figure
	 * out the last record in the data-source is 37, then the other thread
	 * explicitly writes record 37, things will go badly.  I don't want to
	 * lock the data-source on every update, so I'm leaving this until we
	 * write the transactional code, because that's going to change all of
	 * the locking in here.
	 */
	if (cursor->data_source->append_recno == 0) {
		if ((ret = kvs_last(cursor->data_source->kvs,
		    &cursor->record, (unsigned long)0, (unsigned long)0)) != 0)
			goto err;

		if ((ret = wt_ext->struct_unpack(wt_ext, session,
		    r->key, sizeof(r->key),
		    "r", &cursor->data_source->append_recno)) != 0)
			goto err;
	}

	wt_cursor->recno = ++cursor->data_source->append_recno;

err:	ETRET(unlock(session, &cursor->data_source->lock));
	return (ret);
}

/*
 * kvs_cursor_insert --
 *	WT_CURSOR::insert method.
 */
static int
kvs_cursor_insert(WT_CURSOR *wt_cursor)
{
	CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	ret = 0;

	/* Allocate a new record for append operations. */
	if (cursor->config_append && (ret = kvs_recno_alloc(wt_cursor)) != 0)
		return (ret);

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = copyin_val(wt_cursor)) != 0)
		return (ret);

	/*
	 * WT_CURSOR::insert with overwrite set (create the record if it does
	 * not exist, update the record if it does exist), maps to kvs_set.
	 *
	 * WT_CURSOR::insert without overwrite set (create the record if it
	 * does not exist, fail if it does exist), maps to kvs_add.
	 */
	if (cursor->config_overwrite) {
		if ((ret = kvs_set(
		    cursor->data_source->kvs, &cursor->record)) != 0)
			ERET(session, WT_ERROR,
			    "kvs_set: %s", kvs_strerror(ret));
	} else
		if ((ret = kvs_add(
		    cursor->data_source->kvs, &cursor->record)) != 0) {
			if (ret == KVS_E_KEY_EXISTS)
				return (WT_DUPLICATE_KEY);
			ERET(session, WT_ERROR,
			    "kvs_add: %s", kvs_strerror(ret));
		}
	return (0);
}

/*
 * kvs_cursor_update --
 *	WT_CURSOR::update method.
 */
static int
kvs_cursor_update(WT_CURSOR *wt_cursor)
{
	CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = copyin_val(wt_cursor)) != 0)
		return (ret);

	/*
	 * WT_CURSOR::update (update the record if it does exist, fail if it
	 * does not exist), maps to kvs_replace.
	 */
	if ((ret = kvs_replace(cursor->data_source->kvs, &cursor->record)) != 0)
		ERET(session, WT_ERROR, "kvs_replace: %s", kvs_strerror(ret));

	return (0);
}

/*
 * kvs_cursor_remove --
 *	WT_CURSOR::remove method.
 */
static int
kvs_cursor_remove(WT_CURSOR *wt_cursor)
{
	CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	ret = 0;

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of a single byte of zero.
	 */
	if (cursor->config_bitfield) {
		wt_cursor->value.size = 1;
		wt_cursor->value.data = "\0";
		return (kvs_cursor_update(wt_cursor));
	}

	if ((ret = copyin_key(wt_cursor)) != 0)
		return (ret);
	if ((ret = kvs_del(cursor->data_source->kvs, &cursor->record)) == 0)
		return (0);
	if (ret == KVS_E_KEY_NOT_FOUND)
		return (WT_NOTFOUND);
	ERET(session, WT_ERROR, "kvs_del: %s", kvs_strerror(ret));
}

/*
 * kvs_cursor_close --
 *	WT_CURSOR::close method.
 */
static int
kvs_cursor_close(WT_CURSOR *wt_cursor)
{
	CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	cursor = (CURSOR *)wt_cursor;
	session = cursor->session;
	ret = 0;

	if ((ret = writelock(session, &global_lock)) != 0)
		goto err;
	--cursor->data_source->open_cursors;
	if ((ret = unlock(session, &global_lock)) != 0)
		goto err;

err:	free(cursor->val);
	free(cursor);
	return (ret);
}

/*
 * KVS kvs_config structure options.
 */
typedef struct kvs_options {
	const char *name, *type, *checks;
} KVS_OPTIONS;
static const KVS_OPTIONS kvs_options[] = {
	/*
	 * device list
	 */
	{ "kvs_devices=[]",		"list", NULL },

	/*
	 * struct kvs_config configuration
	 */
	{ "kvs_parallelism=64",		"int", "min=1,max=512" },
	{ "kvs_granularity=4000000",	"int", "min=1000000,max=10000000" },
	{ "kvs_avg_key_len=16",		"int", "min=10,max=512" },
	{ "kvs_avg_val_len=16",		"int", "min=10,max=51" },
	{ "kvs_write_bufs=32",		"int", "min=16,max=256" },
	{ "kvs_read_bufs=64",		"int", "min=16,max=256" },
	{ "kvs_commit_timeout=1000000",	"int", "min=100,max=10000000" },
	{ "kvs_reclaim_threshold=60",	"int", "min=1,max=80" },

	/*
	 * KVS_O_FLAG flag configuration
	 */
	{ "kvs_open_o_debug=0",		"boolean", NULL },
	{ "kvs_open_o_reclaim=0",	"boolean", NULL },
	{ "kvs_open_o_scan=0",		"boolean", NULL },

	{ NULL, NULL, NULL }
};

/*
 * kvs_config_add --
 *	Add the KVS configuration options to the WiredTiger configuration
 * process.
 */
static int
kvs_config_add(WT_CONNECTION *conn)
{
	const KVS_OPTIONS *p;
	int ret;

	ret = 0;

	/*
	 * KVS options are currently only allowed on session.create, which means
	 * they cannot be modified for each run, the object create configuration
	 * is permanent.
	 */
	for (p = kvs_options; p->name != NULL; ++p)
		if ((ret = conn->configure_method(conn,
		    "session.create", "memrata:",
		    p->name, p->type, p->checks)) != 0)
			ERET(NULL, ret,
			    "WT_CONNECTION.configure_method: session.create: "
			    "{%s, %s, %s}",
			    p->name, p->type, p->checks, wt_ext->strerror(ret));
	return (0);
}

/*
 * kvs_config_devices --
 *	Convert the device list into an argv[] array.
 */
static int
kvs_config_devices(WT_SESSION *session, WT_CONFIG_ITEM *orig, char ***devices)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_SCAN *scan;
	size_t len;
	u_int cnt, slots;
	int ret;
	char **argv, **p;

	ret = 0;
	argv = NULL;

	/* Set up the scan of the device list. */
	if ((ret = wt_ext->config_scan_begin(
	    wt_ext, session, orig->str, orig->len, &scan)) != 0) {
		ESET(session, ret,
		    "WT_EXTENSION_API::config_scan_begin: %s",
		    wt_ext->strerror(ret));
		return (ret);
	}

	for (cnt = slots = 0; (ret = wt_ext->
	    config_scan_next(wt_ext, scan, &k, &v)) == 0; ++cnt) {
		if (cnt + 1 >= slots) {		/* NULL-terminate the array */
			len = slots + 20 * sizeof(*argv);
			if ((p = realloc(argv, len)) == NULL) {
				ret = os_errno();
				goto err;
			}
			argv = p;
			slots += 20;
		}
		len = k.len + 1;
		if ((argv[cnt] = (char *)calloc(len, sizeof(**argv))) == NULL) {
			ret = os_errno();
			goto err;
		}
		argv[cnt + 1] = NULL;
		memcpy(argv[cnt], k.str, k.len);
	}
	if (ret != WT_NOTFOUND) {
		ESET(session, ret,
		    "WT_EXTENSION_API::config_scan_next: %s",
		    wt_ext->strerror(ret));
		return (ret);
	}
	if ((ret = wt_ext->config_scan_end(wt_ext, scan)) != 0) {
		ESET(session, ret,
		    "WT_EXTENSION_API::config_scan_end: %s",
		    wt_ext->strerror(ret));
		return (ret);
	}

	*devices = argv;
	return (0);

err:	if (argv != NULL) {
		for (p = argv; *p != NULL; ++p)
			free(*p);
		free(argv);
	}
	return (ret);
}

/*
 * kvs_config_read --
 *	Read KVS configuration.
 */
static int
kvs_config_read(WT_SESSION *session, WT_CONFIG_ARG *config,
    char ***devices, struct kvs_config *kvs_config, int *flagsp)
{
	const KVS_OPTIONS *p;
	WT_CONFIG_ITEM v;
	int ret;
	char *t, name[128];

	*flagsp = 0;				/* Clear return values */
	memset(kvs_config, 0, sizeof(*kvs_config));

	ret = 0;

	for (p = kvs_options; p->name != NULL; ++p) {
		/* Truncate the name, discarding the trailing value. */
		(void)snprintf(name, sizeof(name), "%s", p->name);
		if ((t = strchr(name, '=')) != NULL)
			*t = '\0';
		if ((ret =
		    wt_ext->config_get(wt_ext, session, config, name, &v)) != 0)
			ERET(session, ret,
			    "WT_EXTENSION_API.config: %s: %s",
			    name, wt_ext->strerror(ret));

		if (strcmp(name, "kvs_devices") == 0) {
			if ((ret =
			    kvs_config_devices(session, &v, devices)) != 0)
				return (ret);
			continue;
		}

#define	KVS_CONFIG_SET(n, f)						\
		if (strcmp(name, n) == 0) {				\
			kvs_config->f = (unsigned long)v.val;		\
			continue;					\
		}
		KVS_CONFIG_SET("kvs_parallelism", parallelism);
		KVS_CONFIG_SET("kvs_granularity", granularity);
		KVS_CONFIG_SET("kvs_avg_key_len", avg_key_len);
		KVS_CONFIG_SET("kvs_avg_val_len", avg_val_len);
		KVS_CONFIG_SET("kvs_write_bufs", write_bufs);
		KVS_CONFIG_SET("kvs_read_bufs", read_bufs);
		KVS_CONFIG_SET("kvs_commit_timeout", commit_timeout);
		KVS_CONFIG_SET("kvs_reclaim_threshold", reclaim_threshold);

#define	KVS_FLAG_SET(n, f)						\
		if (strcmp(name, n) == 0) {				\
			if (v.val != 0)					\
				*flagsp |= f;				\
			continue;					\
		}
		KVS_FLAG_SET("kvs_open_o_debug", KVS_O_DEBUG);
		KVS_FLAG_SET("kvs_open_o_reclaim", KVS_O_SCAN);
		KVS_FLAG_SET("kvs_open_o_scan",  KVS_O_RECLAIM);
	}
	return (0);
}

/*
 * kvs_create_path_string --
 *	Convert the list of devices into a comma-separate string.   This is
 * only used for error messages, but we want a good error message when an
 * open fails, it's a common error and the user needs to know the list of
 * devices that failed.
 */
static char *
kvs_create_path_string(char **devices)
{
	size_t len;
	char *ebuf, **p;

	for (len = 0, p = devices; *p != NULL; ++p)
		len += strlen(*p) + 5;
	if ((ebuf = malloc(len)) == NULL)
		return (NULL);

	ebuf[0] = '\0';
	for (p = devices; *p != NULL; ++p) {
		(void)strcat(ebuf, *p);
		if (p[1] != NULL)
			(void)strcat(ebuf, ",");
	}
	return (ebuf);
}

/*
 * drop_data_source --
 *	Drop a data source from our list, closing any underlying KVS handle.
 */
static int
drop_data_source(WT_SESSION *session, const char *uri)
{
	DATA_SOURCE *p, **ref;
	int ret;

	if ((ret = writelock(session, &global_lock)) != 0)
		return (ret);

	/* Search our list of objects for a match. */
	for (ref = &data_source_head; (p = *ref) != NULL; p = p->next)
		if (strcmp(p->uri, uri) == 0)
			break;

	/*
	 * If we don't find the URI in our object list, we're done.
	 * If the object is in use (it has open cursors), we can't drop it.
	 * Otherwise, drop it.
	 */
	if (p == NULL || p->open_cursors != 0) {
		if (p != NULL)
			ret = EBUSY;
	} else {
		if ((ret = kvs_close(p->kvs)) != 0)
			ESET(session, WT_ERROR,
			    "kvs_close: %s: %s", uri, kvs_strerror(ret));
		*ref = p->next;

		free(p->uri);
		ETRET(lock_destroy(session, &p->lock));
		free(p);
	}

	ETRET(unlock(session, &global_lock));
	return (ret);
}

/*
 * kvs_session_create --
 *	WT_SESSION::create method.
 */
static int
kvs_session_create(WT_DATA_SOURCE *dsrc,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	struct kvs_config kvs_config;
	kvs_t kvs;
	int flags, ret;
	char **devices, *emsg;

	(void)dsrc;				/* Unused parameters */

	devices = NULL;
	emsg = NULL;

	/*
	 * Check if the object exists and drop it if we can.  What we really
	 * care about is if it's busy, we don't want to continue in the face
	 * of open cursors.
	 */
	if ((ret = drop_data_source(session, uri)) != 0)
		return (ret);

	/* Read the configuration. */
	if ((ret = kvs_config_read(
	    session, config, &devices, &kvs_config, &flags)) != 0)
		goto err;

	/* We require a list of devices underlying the URI. */
	if (devices[0] == NULL) {
		ESET(session, EINVAL,
		    "WT_SESSION.create: no devices specified");
		goto err;
	}

	/*
	 * Open the KVS handle (creating the underlying object), and then close
	 * it, we're done.
	 */
	if ((kvs =
	    kvs_open(devices, &kvs_config, flags | KVS_O_CREATE)) == NULL) {
		emsg = kvs_create_path_string(devices);
		ESET(session, WT_ERROR,
		    "WT_SESSION.create: kvs_open: %s: %s",
		    emsg == NULL ? devices[0] : emsg, kvs_strerror(os_errno()));
		goto err;
	}
	if ((ret = kvs_close(kvs)) != 0) {
		emsg = kvs_create_path_string(devices);
		ESET(session, WT_ERROR,
		    "%s: WT_SESSION.create: kvs_close: %s: %s",
		    emsg == NULL ? devices[0] : emsg, kvs_strerror(ret));
		goto err;
	}

err:	free(emsg);
	free(devices);
	return (ret);
}

/*
 * kvs_session_drop --
 *	WT_SESSION::drop method.
 */
static int
kvs_session_drop(WT_DATA_SOURCE *dsrc,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	(void)dsrc;				/* Unused parameters */
	(void)config;

	/*
	 * Discard the URI from our list.  The KVS object might still exist on
	 * disk, but there's no "drop" operation for KVS, there's no work to do.
	 * Once WiredTiger's meta-data record for the URI is removed, the KVS
	 * object can (must) be re-created, WiredTiger won't allow any other
	 * calls.
	 */
	return (drop_data_source(session, uri));
}

/*
 * open_data_source --
 *	Open a new data source and insert it into the list.
 */
static int
open_data_source(WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	struct kvs_config kvs_config;
	DATA_SOURCE *data_source, *p;
	int ds_lockinit, flags, locked, ret;
	char **devices, *emsg;

	devices = NULL;
	emsg = NULL;
	ds_lockinit = locked = ret = 0;

	/*
	 * The first time we open a cursor on an object, allocate an underlying
	 * data source object.
	 */
	if ((data_source =
	    (DATA_SOURCE *)calloc(1, sizeof(DATA_SOURCE))) == NULL)
		return (os_errno());
	if ((data_source->uri = strdup(uri)) == NULL)
		goto err;
	if ((ret = lock_init(session, &data_source->lock)) != 0)
		goto err;
	ds_lockinit = 1;

	/* Read the configuration. */
	if ((ret = kvs_config_read(
	    session, config, &devices, &kvs_config, &flags)) != 0)
		goto err;

	/* We require a list of devices underlying the URI. */
	if (devices[0] == NULL) {
		ESET(
		    session, EINVAL, "WT_SESSION.create: no devices specified");
		goto err;
	}

	/*
	 * kvs_open isn't re-entrant: lock things down while we make sure we
	 * don't have more than a single handle at a time.
	 */
	if ((ret = writelock(session, &global_lock)) != 0)
		goto err;
	locked = 1;

	/*
	 * Check for a match: if we find one, we raced, but we return success,
	 * someone else did the work.
	 */
	for (p = data_source_head; p != NULL; p = p->next)
		if (strcmp(p->uri, uri) == 0)
			goto err;

	/* Open the KVS handle. */
	if ((data_source->kvs =
	    kvs_open(devices, &kvs_config, flags)) == NULL) {
		emsg = kvs_create_path_string(devices);
		ESET(session, WT_ERROR,
		    "WT_SESSION.create: kvs_open: %s: %s",
		    emsg == NULL ? devices[0] : emsg, kvs_strerror(os_errno()));
		goto err;
	}

	/* Insert the new entry at the head of the list. */
	data_source->next = data_source_head;
	data_source_head = data_source;
	data_source = NULL;

err:	if (locked)
		ETRET(unlock(session, &global_lock));

	if (data_source != NULL) {
		if (data_source->uri != NULL)
			free(data_source->uri);
		if (ds_lockinit)
			ETRET(lock_destroy(session, &data_source->lock));
		free(data_source);
	}
	free(devices);
	free(emsg);
	return (ret);
}

/*
 * kvs_session_open_cursor --
 *	WT_SESSION::open_cursor method.
 */
static int
kvs_session_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, WT_CURSOR **new_cursor)
{
	CURSOR *cursor;
	DATA_SOURCE *p;
	WT_CONFIG_ITEM v;
	int ret;

	(void)dsrc;				/* Unused parameters */

	cursor = NULL;
	ret = 0;

	/* Allocate and initialize a cursor. */
	if ((cursor = (CURSOR *)calloc(1, sizeof(CURSOR))) == NULL)
		return (os_errno());

	cursor->session = session;
	cursor->record.key = cursor->key;
	if ((cursor->val = malloc(128)) == NULL)
		goto err;
	cursor->val_len = 128;
						/* Initialize the methods */
	cursor->wt_cursor.next = kvs_cursor_next;
	cursor->wt_cursor.prev = kvs_cursor_prev;
	cursor->wt_cursor.reset = kvs_cursor_reset;
	cursor->wt_cursor.search = kvs_cursor_search;
	cursor->wt_cursor.search_near = kvs_cursor_search_near;
	cursor->wt_cursor.insert = kvs_cursor_insert;
	cursor->wt_cursor.update = kvs_cursor_update;
	cursor->wt_cursor.remove = kvs_cursor_remove;
	cursor->wt_cursor.close = kvs_cursor_close;

						/* Parse configuration */
	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "append", &v)) != 0) {
		ESET(session, ret,
		    "append configuration: %s", wt_ext->strerror(ret));
		goto err;
	}
	cursor->config_append = v.val != 0;

	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "overwrite", &v)) != 0) {
		ESET(session, ret,
		    "overwrite configuration: %s", wt_ext->strerror(ret));
		goto err;
	}
	cursor->config_overwrite = v.val != 0;

	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "key_format", &v)) != 0) {
		ESET(session, ret,
		    "key_format configuration: %s", wt_ext->strerror(ret));
		goto err;
	}
	cursor->config_recno = v.len == 1 && v.str[0] == 'r';

	if ((ret = wt_ext->config_get(
	    wt_ext, session, config, "value_format", &v)) != 0) {
		ESET(session, ret,
		    "value_format configuration: %s", wt_ext->strerror(ret));
		goto err;
	}
	cursor->config_bitfield =
	    v.len == 2 && isdigit(v.str[0]) && v.str[1] == 't';

	/*
	 * See if the object already exists: if it does, increment the count of
	 * open cursors to pin it, and release the lock.
	 */
	for (;;) {
		if ((ret = writelock(session, &global_lock)) != 0)
			goto err;
		for (p = data_source_head; p != NULL; p = p->next)
			if (strcmp(p->uri, uri) == 0) {
				++p->open_cursors;
				break;
			}
		if ((ret = unlock(session, &global_lock)) != 0)
			goto err;
		if (p != NULL) {
			cursor->data_source = p;

			*new_cursor = (WT_CURSOR *)cursor;
			return (0);
		}

		/* Create the object. */
		if ((ret = open_data_source(session, uri, config)) != 0)
			goto err;

		/*
		 * We shouldn't loop more than once, but it's theoretically
		 * possible.
		 */
	}

err:	if (cursor != NULL) {
		if (cursor->val != NULL)
			free(cursor->val);
		free(cursor);
	}
	return (ret);
}

/*
 * kvs_session_rename --
 *	WT_SESSION::rename method.
 */
static int
kvs_session_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *newname, WT_CONFIG_ARG *config)
{
	(void)dsrc;				/* Unused parameters */
	(void)newname;
	(void)config;

	/* Discard the URI from our list. */
	return (drop_data_source(session, uri));
}

/*
 * kvs_session_truncate --
 *	WT_SESSION::truncate method.
 */
static int
kvs_session_truncate(WT_DATA_SOURCE *dsrc,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *p;
	WT_CURSOR *wt_cursor;
	int ret, tret;

	/*
	 * Truncate should work even if the object is not yet opened: if we
	 * don't find it, open it.   We loop because we could theoretically 
	 * race with other threads creating/deleting the object.
	 */
	for (;;) {
		if ((ret = writelock(session, &global_lock)) != 0)
			return (ret);

		/* Search our list of objects for a match. */
		for (p = data_source_head; p != NULL; p = p->next)
			if (strcmp(p->uri, uri) == 0)
				break;

		/* If we don't find the object, open it. */
		if (p == NULL) {
			if ((ret = unlock(session, &global_lock)) != 0)
				return (ret);
			if ((ret = open_data_source(session, uri, config)) != 0)
				return (ret);
			continue;
		}
		if (p->open_cursors == 0) {
			if ((tret = kvs_truncate(p->kvs)) != 0)
				ESET(NULL, WT_ERROR,
				    "kvs_truncate: %s: %s",
				    p->uri, kvs_strerror(tret));
		} else
			ret = EBUSY;
		ETRET(unlock(session, &global_lock));
		return (ret);
	}
	/* NOTREACHED */
}

/*
 * kvs_session_verify --
 *	WT_SESSION::verify method.
 */
static int
kvs_session_verify(WT_DATA_SOURCE *dsrc,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	(void)dsrc;				/* Unused parameters */
	(void)uri;
	(void)config;

	ERET(session, ENOTSUP, "verify: %s", strerror(ENOTSUP));
}

/*
 * wiredtiger_extension_init --
 *	Initialize the KVS connector code.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	static WT_DATA_SOURCE data_source;	/* Enclosing data source */
	int ret;

	(void)config;				/* Unused parameters */

	ret = 0;
						/* Acquire the extension API */
	wt_ext = connection->get_extension_api(connection);

						/* Check the library version */
#if KVS_VERSION_MAJOR != 2 || KVS_VERSION_MINOR != 8
	ERET(NULL, EINVAL,
	    "unsupported KVS library version %d.%d",
	    KVS_VERSION_MAJOR, KVS_VERSION_MINOR);
#endif

	/* Initialize the WT_DATA_SOURCE structure. */
	memset(&data_source, 0, sizeof(data_source));
	data_source.create = kvs_session_create;
	data_source.compact = NULL;		/* No compaction */
	data_source.drop = kvs_session_drop;
	data_source.open_cursor = kvs_session_open_cursor;
	data_source.rename = kvs_session_rename;
	data_source.salvage = NULL;		/* No salvage */
	data_source.truncate = kvs_session_truncate;
	data_source.verify = kvs_session_verify;
	if ((ret = connection->add_data_source(
	    connection, "memrata:", &data_source, NULL)) != 0)
		ERET(NULL, ret,
		    "WT_CONNECTION.add_data_source: %s", wt_ext->strerror(ret));

	/* Add the KVS-specific configuration options. */
	if ((ret = kvs_config_add(connection)) != 0)
		return (ret);

	if ((ret = lock_init(NULL, &global_lock)) != 0)
		return (ret);

	return (0);
}

/*
 * wiredtiger_extension_terminate --
 *	Shutdown the KVS connector code.
 */
int
wiredtiger_extension_terminate(WT_CONNECTION *connection)
{
	DATA_SOURCE *p;
	int ret, tret;

	(void)connection;			/* Unused parameters */

	ret = writelock(NULL, &global_lock);

	/* Start a flush on any open objects. */
	for (p = data_source_head; p != NULL; p = p->next)
		if ((tret = kvs_commit(p->kvs)) != 0)
			ESET(NULL, WT_ERROR,
			    "kvs_commit: %s: %s", p->uri, kvs_strerror(tret));

	/* Complain if any of the objects are in use. */
	for (p = data_source_head; p != NULL; p = p->next)
		if (p->open_cursors != 0)
			ESET(NULL, WT_ERROR,
			    "%s: has open cursors during close", p->uri);

	/* Close and discard the remaining objects. */
	while ((p = data_source_head) != NULL) {
		if ((tret = kvs_close(p->kvs)) != 0)
			ESET(NULL, WT_ERROR,
			    "kvs_close: %s: %s", p->uri, kvs_strerror(tret));
		data_source_head = p->next;
		free(p->uri);
		ETRET(lock_destroy(NULL, &p->lock));
		free(p);
	}

	ETRET(unlock(NULL, &global_lock));
	ETRET(lock_destroy(NULL, &global_lock));

	wt_ext = NULL;

	return (ret);
}
