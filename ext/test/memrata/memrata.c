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
#define	ERET(wtext, session, v, ...) do {				\
	(void)								\
	    wtext->err_printf(wtext, session, "memrata: " __VA_ARGS__);	\
	return (v);							\
} while (0)
#undef	ETRET
#define	ETRET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0 &&					\
	    (__ret == WT_PANIC ||					\
	    ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND))	\
		ret = __ret;						\
} while (0)
#undef	ESET
#define	ESET(wtext, session, v, ...) do {				\
	(void)								\
	    wtext->err_printf(wtext, session, "memrata: " __VA_ARGS__);	\
	ETRET(v);							\
} while (0)

/*
 * Version each file, out of sheer raging paranoia.
 */
#define	KVS_MAJOR	1			/* KVS major, minor version */
#define	KVS_MINOR	0

typedef struct __wt_source {
	char *uri;				/* Unique name */
	pthread_rwlock_t lock;			/* Lock */

	int	configured;			/* If structure configured */
	u_int	ref;				/* Active reference count */

	uint64_t append_recno;			/* Allocation record number */

	int	 config_recno;			/* config "key_format=r" */
	int	 config_bitfield;		/* config "value_format=#t" */

	kvs_t kvs;				/* Underlying KVS namespace */
	struct __kvs_source *ks;		/* Underlying KVS source */

	struct __wt_source *next;		/* List of WiredTiger objects */
} WT_SOURCE;

typedef struct __kvs_source {
	char *name;				/* Unique name */
	kvs_t kvs_device;			/* Underlying KVS store */

	struct __wt_source *ws_head;		/* List of WiredTiger sources */

	struct __kvs_source *next;		/* List of KVS sources */
} KVS_SOURCE;

typedef struct __cursor {
	WT_CURSOR wtcursor;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	WT_SOURCE *ws;				/* WiredTiger source */

	struct kvs_record record;		/* Record */
	uint8_t  key[KVS_MAX_KEY_LEN];		/* key, value */
	uint8_t *val;
	size_t   val_len;

	int	 config_append;			/* config "append" */
	int	 config_overwrite;		/* config "overwrite" */
} CURSOR;

typedef struct __data_source {
	WT_DATA_SOURCE wtds;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	pthread_rwlock_t global_lock;		/* Global lock */

	KVS_SOURCE *kvs_head;			/* List of KVS sources */
} DATA_SOURCE;

/*
 * os_errno --
 *	Limit our use of errno so it's easy to remove.
 */
static int
os_errno(void)
{
	return (errno);
}

/*
 * lock_init --
 *	Initialize a lock.
 */
static int
lock_init(
    WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_init(lockp, NULL)) != 0)
		ERET(wtext, session, WT_PANIC, "lock init: %s", strerror(ret));
	return (0);
}

/*
 * lock_destroy --
 *	Destroy a lock.
 */
static int
lock_destroy(
    WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_destroy(lockp)) != 0)
		ERET(wtext,
		    session, WT_PANIC, "lock destroy: %s", strerror(ret));
	return (0);
}

/*
 * writelock --
 *	Acquire a write lock.
 */
static INLINE int
writelock(
    WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_wrlock(lockp)) != 0)
		ERET(wtext,
		    session, WT_PANIC, "write-lock: %s", strerror(ret));
	return (0);
}

/*
 * unlock --
 *	Release a lock.
 */
static INLINE int
unlock(WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_unlock(lockp)) != 0)
		ERET(wtext, session, WT_PANIC, "unlock: %s", strerror(ret));
	return (0);
}

/*
 * copyin_key --
 *	Copy a WT_CURSOR key to a struct kvs_record key.
 */
static INLINE int
copyin_key(WT_CURSOR *wtcursor, int allocate_key)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	size_t size;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	ws = cursor->ws;
	wtext = cursor->wtext;

	r = &cursor->record;
	if (ws->config_recno) {
		/*
		 * Allocate a new record for append operations.
		 *
		 * A specified record number could potentially be larger than
		 * the maximum known record number, update the maximum number
		 * as necessary.
		 *
		 * XXX
		 * Assume we can compare 8B values without locking them, and
		 * test again after acquiring the lock.
		 *
		 * XXX
		 * If the put fails for some reason, we'll have incremented the
		 * maximum record number past the correct point.  I can't think
		 * of a reason any application would care or notice, but it's
		 * not quite right.
		 */
		if (allocate_key && cursor->config_append) {
			if ((ret = writelock(wtext, session, &ws->lock)) != 0)
				return (ret);
			wtcursor->recno = ++ws->append_recno;
			if ((ret = unlock(wtext, session, &ws->lock)) != 0)
				return (ret);
		} else if (wtcursor->recno > ws->append_recno) {
			if ((ret = writelock(wtext, session, &ws->lock)) != 0)
				return (ret);
			if (wtcursor->recno > ws->append_recno)
				ws->append_recno = wtcursor->recno;
			if ((ret = unlock(wtext, session, &ws->lock)) != 0)
				return (ret);
		}

		if ((ret = wtext->struct_size(wtext, session,
		    &size, "r", wtcursor->recno)) != 0 ||
		    (ret = wtext->struct_pack(wtext, session, cursor->key,
		    sizeof(cursor->key), "r", wtcursor->recno)) != 0)
			return (ret);
		r->key = cursor->key;
		r->key_len = size;
	} else {
		if (wtcursor->key.size > KVS_MAX_KEY_LEN)
			ERET(wtext, session, ERANGE,
			    "key size of %" PRIuMAX " is too large",
			    (uintmax_t)wtcursor->key.size);

		/*
		 * XXX
		 * The underlying KVS library data fields aren't const.
		 */
		r->key = (char *)wtcursor->key.data;
		r->key_len = wtcursor->key.size;
	}
	return (0);
}

/*
 * copyout_key --
 *	Copy a struct kvs_record key to a WT_CURSOR key.
 */
static INLINE int
copyout_key(WT_CURSOR *wtcursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;

	r = &cursor->record;
	if (ws->config_recno) {
		if ((ret = wtext->struct_unpack(wtext,
		    session, r->key, r->key_len, "r", &wtcursor->recno)) != 0)
			return (ret);
	} else {
		wtcursor->key.data = r->key;
		wtcursor->key.size = (uint32_t)r->key_len;
	}
	return (0);
}

/*
 * copyin_val --
 *	Copy a WT_CURSOR value to a struct kvs_record value .
 */
static INLINE int
copyin_val(WT_CURSOR *wtcursor)
{
	struct kvs_record *r;
	CURSOR *cursor;

	cursor = (CURSOR *)wtcursor;
	r = &cursor->record;

	/*
	 * XXX
	 * The underlying KVS library data fields aren't const.
	 */
	r->val = (uint8_t *)wtcursor->value.data;
	r->val_len = (unsigned long)wtcursor->value.size;
	return (0);
}

/*
 * copyout_val --
 *	Copy a struct kvs_record value to a WT_CURSOR value.
 */
static INLINE int
copyout_val(WT_CURSOR *wtcursor)
{
	struct kvs_record *r;
	CURSOR *cursor;

	cursor = (CURSOR *)wtcursor;
	r = &cursor->record;

	wtcursor->value.data = r->val;
	wtcursor->value.size = (uint32_t)r->val_len;
	return (0);
}

/*
 * copy_key --
 *	Copy the key for methods where the underlying KVS call returns a key.
 */
static INLINE int
copy_key(WT_CURSOR *wtcursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	r = &cursor->record;

	if (r->key == cursor->key)
		return (0);

	if (r->key_len > sizeof(cursor->key))
		ERET(wtext, session, ERANGE,
		    "key too large, maximum is %" PRIuMAX,
		    (uintmax_t)sizeof(cursor->key));

	memcpy(cursor->key, r->key, r->key_len);
	r->key = cursor->key;
	return (0);
}

#if 0
/*
 * kvs_dump --
 *	Dump the records in the KVS store.
 */
static void
kvs_dump(
    WT_EXTENSION_API *wtext, WT_SESSION *session, kvs_t kvs, const char *tag)
{
	struct kvs_record *r, _r;
	uint64_t recno;
	size_t len, size;
	uint8_t *p, key[256], val[256];

	printf("== %s\n", tag);

	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	r->key_len = 0;
	r->val = val;
	r->val_len = sizeof(val);
	while (kvs_next(kvs, r, 0UL, (unsigned long)sizeof(val)) == 0) {
		p = r->key;
		len = r->key_len;
		(void)wtext->struct_unpack(wtext, session, p, 10, "r", &recno);
		(void)wtext->struct_size(wtext, session, &size, "r", recno);
		printf("\t%" PRIu64 ": ", recno);
		printf("%.*s/%.*s\n",
		    (int)(len - size), p + size,
		    (int)r->val_len, (char *)r->val);

		r->val_len = sizeof(val);
	}
}
#endif

/*
 * kvs_call --
 *	Call a KVS key retrieval function, handling overflow.
 */
static INLINE int
kvs_call(WT_CURSOR *wtcursor, const char *fname,
    int (*f)(kvs_t, struct kvs_record *, unsigned long, unsigned long))
{
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	kvs_t kvs;
	int ret = 0;
	char *p;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	kvs = cursor->ws->kvs;
	cursor->record.val = cursor->val;

restart:
	if ((ret = f(kvs, &cursor->record,
	    0UL, (unsigned long)cursor->val_len)) != 0) {
		if (ret == KVS_E_KEY_NOT_FOUND)
			return (WT_NOTFOUND);
		ERET(wtext,
		    session, WT_ERROR, "%s: %s", fname, kvs_strerror(ret));
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
		    0UL, (unsigned long)cursor->val_len)) != 0) {
			if (ret == KVS_E_KEY_NOT_FOUND)
				goto restart;
			ERET(wtext, session,
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
kvs_cursor_next(WT_CURSOR *wtcursor)
{
	int ret = 0;

	if ((ret = copy_key(wtcursor)) != 0)
		return (ret);
	if ((ret = kvs_call(wtcursor, "kvs_next", kvs_next)) != 0)
		return (ret);
	if ((ret = copyout_key(wtcursor)) != 0)
		return (ret);
	if ((ret = copyout_val(wtcursor)) != 0)
		return (ret);
	return (0);
}

/*
 * kvs_cursor_prev --
 *	WT_CURSOR::prev method.
 */
static int
kvs_cursor_prev(WT_CURSOR *wtcursor)
{
	int ret = 0;

	if ((ret = copy_key(wtcursor)) != 0)
		return (ret);
	if ((ret = kvs_call(wtcursor, "kvs_prev", kvs_prev)) != 0)
		return (ret);
	if ((ret = copyout_key(wtcursor)) != 0)
		return (ret);
	if ((ret = copyout_val(wtcursor)) != 0)
		return (ret);
	return (0);
}

/*
 * kvs_cursor_reset --
 *	WT_CURSOR::reset method.
 */
static int
kvs_cursor_reset(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;

	cursor = (CURSOR *)wtcursor;

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
kvs_cursor_search(WT_CURSOR *wtcursor)
{
	int ret = 0;

	if ((ret = copyin_key(wtcursor, 0)) != 0)
		return (ret);
	if ((ret = kvs_call(wtcursor, "kvs_get", kvs_get)) != 0)
		return (ret);
	if ((ret = copyout_val(wtcursor)) != 0)
		return (ret);
	return (0);
}

/*
 * kvs_cursor_search_near --
 *	WT_CURSOR::search_near method.
 */
static int
kvs_cursor_search_near(WT_CURSOR *wtcursor, int *exact)
{
	int ret = 0;

	/*
	 * XXX
	 * I'm not confident this is sufficient: if there are multiple threads
	 * of control, it's possible for the search for an exact match to fail,
	 * another thread of control to insert (and commit) an exact match, and
	 * then it's possible we'll return the wrong value.  This needs to be
	 * revisited once the transactional code is in place.
	 */

	/* Search for an exact match. */
	if ((ret = kvs_cursor_search(wtcursor)) == 0) {
		*exact = 0;
		return (0);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	/* Search for a key that's larger. */
	if ((ret = kvs_cursor_next(wtcursor)) == 0) {
		*exact = 1;
		return (0);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	/* Search for a key that's smaller. */
	if ((ret = kvs_cursor_prev(wtcursor)) == 0) {
		*exact = -1;
		return (0);
	}

	return (ret);
}

/*
 * kvs_cursor_insert --
 *	WT_CURSOR::insert method.
 */
static int
kvs_cursor_insert(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;

	if ((ret = copyin_key(wtcursor, 1)) != 0)
		return (ret);
	if ((ret = copyin_val(wtcursor)) != 0)
		return (ret);

	/*
	 * WT_CURSOR::insert with overwrite set (create the record if it does
	 * not exist, update the record if it does exist), maps to kvs_set.
	 *
	 * WT_CURSOR::insert without overwrite set (create the record if it
	 * does not exist, fail if it does exist), maps to kvs_add.
	 */
	if (cursor->config_overwrite) {
		if ((ret = kvs_set(ws->kvs, &cursor->record)) != 0)
			ERET(wtext, session, WT_ERROR,
			    "kvs_set: %s", kvs_strerror(ret));
	} else
		if ((ret = kvs_add(ws->kvs, &cursor->record)) != 0) {
			if (ret == KVS_E_KEY_EXISTS)
				return (WT_DUPLICATE_KEY);
			ERET(wtext, session, WT_ERROR,
			    "kvs_add: %s", kvs_strerror(ret));
		}
	return (0);
}

/*
 * kvs_cursor_update --
 *	WT_CURSOR::update method.
 */
static int
kvs_cursor_update(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;

	if ((ret = copyin_key(wtcursor, 0)) != 0)
		return (ret);
	if ((ret = copyin_val(wtcursor)) != 0)
		return (ret);

	/*
	 * WT_CURSOR::update (update the record if it does exist, fail if it
	 * does not exist), maps to kvs_replace.
	 */
	if ((ret = kvs_replace(ws->kvs, &cursor->record)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "kvs_replace: %s", kvs_strerror(ret));

	return (0);
}

/*
 * kvs_cursor_remove --
 *	WT_CURSOR::remove method.
 */
static int
kvs_cursor_remove(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of a single byte of zero.
	 */
	if (ws->config_bitfield) {
		wtcursor->value.size = 1;
		wtcursor->value.data = "\0";
		return (kvs_cursor_update(wtcursor));
	}

	if ((ret = copyin_key(wtcursor, 0)) != 0)
		return (ret);
	if ((ret = kvs_del(ws->kvs, &cursor->record)) == 0)
		return (0);
	if (ret == KVS_E_KEY_NOT_FOUND)
		return (WT_NOTFOUND);
	ERET(wtext, session, WT_ERROR, "kvs_del: %s", kvs_strerror(ret));
}

/*
 * kvs_cursor_close --
 *	WT_CURSOR::close method.
 */
static int
kvs_cursor_close(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;

	if ((ret = writelock(wtext, session, &ws->lock)) != 0)
		goto err;
	--ws->ref;
	if ((ret = unlock(wtext, session, &ws->lock)) != 0)
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
	{ "kvs_granularity=2M",		"int", "min=1M,max=10M" },
	{ "kvs_avg_key_len=16",		"int", "min=10,max=512" },
	{ "kvs_avg_val_len=100",	"int", "min=10,max=1M" },
	{ "kvs_write_bufs=32",		"int", "min=16,max=256" },
	{ "kvs_read_bufs=2048",		"int", "min=16,max=1M" },
	{ "kvs_commit_timeout=5M",	"int", "min=100,max=10M" },
	{ "kvs_reclaim_threshold=60",	"int", "min=1,max=80" },
	{ "kvs_reclaim_period=1000",	"int", "min=100,max=10K" },

	/*
	 * KVS_O_FLAG flag configuration
	 */
	{ "kvs_open_o_debug=0",		"boolean", NULL },
	{ "kvs_open_o_truncate=0",	"boolean", NULL },

	{ NULL, NULL, NULL }
};

/*
 * kvs_config_add --
 *	Add the KVS configuration options to the WiredTiger configuration
 * process.
 */
static int
kvs_config_add(WT_CONNECTION *connection, WT_EXTENSION_API *wtext)
{
	const KVS_OPTIONS *p;
	const char *methods[] = {
	    "session.create",
	    "session.drop",
	    "session.open_cursor",
	    "session.rename",
	    "session.truncate",
	    "session.verify",
	    NULL
	}, **method;
	int ret = 0;

	/*
	 * All WiredTiger methods taking a URI must support kvs_devices, that's
	 * how we name the underlying store.
	 *
	 * Additionally, any method taking a URI might be the first open of the
	 * store, and we cache store handles, which means the first entrant gets
	 * to configure the underlying device.  That's wrong: if we ever care,
	 * we should check any method taking configuration options against the
	 * already configured values, failing attempts to change configuration
	 * on a cached handle.  There's no other fix, KVS configuration is tied
	 * to device open.
	 */
	for (method = methods; *method != NULL; ++method)
		for (p = kvs_options; p->name != NULL; ++p)
			if ((ret = connection->configure_method(connection,
			    *method,
			    "memrata:", p->name, p->type, p->checks)) != 0)
				ERET(wtext, NULL, ret,
				    "WT_CONNECTION.configure_method: "
				    "%s: {%s, %s, %s}",
				    *method, p->name,
				    p->type, p->checks, wtext->strerror(ret));
	return (0);
}

/*
 * kvs_config_devices --
 *	Convert the device list into an argv[] array.
 */
static int
kvs_config_devices(WT_EXTENSION_API *wtext,
    WT_SESSION *session, WT_CONFIG_ITEM *orig, char ***devices)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_SCAN *scan;
	size_t len;
	u_int cnt, slots;
	int ret = 0;
	char **argv, **p;

	argv = NULL;

	/* Set up the scan of the device list. */
	if ((ret = wtext->config_scan_begin(
	    wtext, session, orig->str, orig->len, &scan)) != 0) {
		ESET(wtext, session, ret,
		    "WT_EXTENSION_API::config_scan_begin: %s",
		    wtext->strerror(ret));
		goto err;
	}

	for (cnt = slots = 0; (ret = wtext->
	    config_scan_next(wtext, scan, &k, &v)) == 0; ++cnt) {
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
		if ((argv[cnt] = calloc(len, sizeof(**argv))) == NULL) {
			ret = os_errno();
			goto err;
		}
		argv[cnt + 1] = NULL;
		memcpy(argv[cnt], k.str, k.len);
	}
	if (ret != WT_NOTFOUND) {
		ESET(wtext, session, ret,
		    "WT_EXTENSION_API::config_scan_next: %s",
		    wtext->strerror(ret));
		goto err;
	}
	if ((ret = wtext->config_scan_end(wtext, scan)) != 0) {
		ESET(wtext, session, ret,
		    "WT_EXTENSION_API::config_scan_end: %s",
		    wtext->strerror(ret));
		goto err;
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
kvs_config_read(WT_EXTENSION_API *wtext,
    WT_SESSION *session, WT_CONFIG_ARG *config,
    char ***devices, struct kvs_config *kvs_config, int *flagsp)
{
	const KVS_OPTIONS *p;
	WT_CONFIG_ITEM v;
	int ret = 0;
	char *t, name[128];

	*flagsp = 0;				/* Clear return values */
	memset(kvs_config, 0, sizeof(*kvs_config));

	for (p = kvs_options; p->name != NULL; ++p) {
		/* Truncate the name, discarding the trailing value. */
		(void)snprintf(name, sizeof(name), "%s", p->name);
		if ((t = strchr(name, '=')) != NULL)
			*t = '\0';
		if ((ret =
		    wtext->config_get(wtext, session, config, name, &v)) != 0)
			ERET(wtext, session, ret,
			    "WT_EXTENSION_API.config: %s: %s",
			    name, wtext->strerror(ret));

		if (strcmp(name, "kvs_devices") == 0) {
			if ((ret = kvs_config_devices(
			    wtext, session, &v, devices)) != 0)
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
		KVS_CONFIG_SET("kvs_reclaim_period", reclaim_period);

#define	KVS_FLAG_SET(n, f)						\
		if (strcmp(name, n) == 0) {				\
			if (v.val != 0)					\
				*flagsp |= f;				\
			continue;					\
		}
		/*
		 * We don't export KVS_O_CREATE: WT_SESSION.create always adds
		 * it in.
		 */
		KVS_FLAG_SET("kvs_open_o_debug", KVS_O_DEBUG);
		KVS_FLAG_SET("kvs_open_o_truncate",  KVS_O_TRUNCATE);
	}
	return (0);
}

/*
 * device_list_sort --
 *	Sort the list of devices.
 */
static void
device_list_sort(char **device_list)
{
	char **p, **t, *tmp;

	/* Sort the list of devices. */
	for (p = device_list; *p != NULL; ++p)
		for (t = p + 1; *t != NULL; ++t)
			if (strcmp(*p, *t) > 0) {
				tmp = *p;
				*p = *t;
				*t = tmp;
			}
}

/*
 * device_string --
 *	Convert the list of devices into a comma-separated string.
 */
static int
device_string(char **device_list, char **devicesp)
{
	size_t len;
	char **p, *tmp;

	/* Allocate a buffer and build the name. */
	for (len = 0, p = device_list; *p != NULL; ++p)
		len += strlen(*p) + 5;
	if ((tmp = malloc(len)) == NULL)
		return (os_errno());

	tmp[0] = '\0';
	for (p = device_list; *p != NULL; ++p) {
		(void)strcat(tmp, *p);
		if (p[1] != NULL)
			(void)strcat(tmp, ",");
	}
	*devicesp = tmp;
	return (0);
}

/*
 * kvs_source_open --
 *	Return a locked KVS source, allocating and opening if it doesn't already
 * exist.
 */
static int
kvs_source_open(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, WT_CONFIG_ARG *config, KVS_SOURCE **refp)
{
	struct kvs_config kvs_config;
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	int flags, locked, ret = 0;
	char **device_list, *devices, **p;

	*refp = NULL;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	ks = NULL;
	device_list = NULL;
	devices = NULL;
	locked = 0;

	/*
	 * Read the configuration.  We require a list of devices underlying the
	 * KVS source, parse the device list found in the configuration string
	 * into an array of paths.
	 *
	 * That list of devices is a unique name.  Sort the list of devices so
	 * ordering doesn't matter in naming, then create a "name" string for
	 * the device so we can uniquely identify it.  The reason we go from a
	 * configuration string to an array and then back to another string is
	 * because we want to be sure the order the devices are listed in the
	 * string doesn't change the "real" name of the store.
	 */
	if ((ret = kvs_config_read(wtext,
	    session, config, &device_list, &kvs_config, &flags)) != 0)
		goto err;
	if (device_list == NULL || device_list[0] == NULL) {
		ESET(wtext, session, EINVAL, "kvs_open: no devices specified");
		goto err;
	}
	device_list_sort(device_list);
	if ((ret = device_string(device_list, &devices)) != 0)
		goto err;

	/*
	 * kvs_open isn't re-entrant and you can't open a handle multiple times,
	 * lock things down while we check.
	 */
	if ((ret = writelock(wtext, session, &ds->global_lock)) != 0)
		goto err;
	locked = 1;

	/* Check for a match: if we find one we're done. */
	for (ks = ds->kvs_head; ks != NULL; ks = ks->next)
		if (strcmp(ks->name, devices) == 0)
			goto done;

	/* Allocate and initialize a new underlying KVS source object. */
	if ((ks = calloc(1, sizeof(*ks))) == NULL) {
		ret = os_errno();
		goto err;
	}
	ks->name = devices;
	devices = NULL;

	/*
	 * Open the underlying KVS store (creating it if necessary), then push
	 * the change.
	 */
	ks->kvs_device =
	    kvs_open(device_list, &kvs_config, flags | KVS_O_CREATE);
	if (ks->kvs_device == NULL) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_open: %s: %s", ks->name, kvs_strerror(os_errno()));
		goto err;
	}
	if ((ret = kvs_commit(ks->kvs_device)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_commit: %s", kvs_strerror(ret));
		goto err;
	}

	/* Insert the new entry at the head of the list. */
	ks->next = ds->kvs_head;
	ds->kvs_head = ks;

	/* Return the locked object. */
done:	*refp = ks;
	ks = NULL;

	if (0) {
err:		if (locked)
			ETRET(unlock(wtext, session, &ds->global_lock));
	}

	if (ks != NULL) {
		if (ks->kvs_device != NULL)
			(void)kvs_close(ks->kvs_device);
		free(ks);
	}
	if (device_list != NULL) {
		for (p = device_list; *p != NULL; ++p)
			free(*p);
		free(device_list);
	}
	free(devices);
	return (ret);
}

/*
 * ws_source_open --
 *	Return a locked WiredTiger source, allocating and opening if it doesn't
 * already exist.
 */
static int
ws_source_open(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    WT_CONFIG_ARG *config, const char *uri, int hold_global, WT_SOURCE **refp)
{
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int lockinit, ret = 0;

	*refp = NULL;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	lockinit = 0;

	/* Get the underlying KVS source. */
	if ((ret = kvs_source_open(wtds, session, config, &ks)) != 0)
		return (ret);

	/* Check for a match: if we find one, we're done. */
	for (ws = ks->ws_head; ws != NULL; ws = ws->next)
		if (strcmp(ws->uri, uri) == 0)
			goto done;

	/* Allocate and initialize a new underlying WiredTiger source object. */
	if ((ws = calloc(1, sizeof(*ws))) == NULL ||
	    (ws->uri = strdup(uri)) == NULL) {
		ret = os_errno();
		goto err;
	}
	if ((ret = lock_init(wtext, session, &ws->lock)) != 0)
		goto err;
	lockinit = 1;
	ws->ks = ks;

	/*
	 * Open the underlying KVS namespace (creating it if necessary), then
	 * push the change.
	 */
	if ((ws->kvs = kvs_open_namespace(
	    ws->ks->kvs_device, uri, KVS_O_CREATE)) == NULL) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_open_namespace: %s: %s",
		    uri, kvs_strerror(os_errno()));
		goto err;
	}
	if ((ret = kvs_commit(ws->kvs)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_commit: %s", kvs_strerror(ret));
		goto err;
	}

	/* Insert the new entry at the head of the list. */
	ws->next = ks->ws_head;
	ks->ws_head = ws;

	/* If we're not holding the global lock, lock the object. */
done:	if (!hold_global && (ret = writelock(wtext, session, &ws->lock)) != 0)
		goto err;

	*refp = ws;
	ws = NULL;

	if (0) {
err:		if (lockinit)
			ETRET(lock_destroy(wtext, session, &ws->lock));
		if (ws != NULL) {
			if (ws->kvs != NULL)
				(void)kvs_close(ws->kvs);
			free(ws->uri);
			free(ws);
		}
	}

	/* If our caller doesn't need it, release the global lock. */
	if (ret != 0 || !hold_global)
		ETRET(unlock(wtext, session, &ds->global_lock));
	return (ret);
}

/*
 * ws_source_destroy --
 *	Kill a WT_SOURCE structure.
 */
static int
ws_source_destroy(WT_EXTENSION_API *wtext, WT_SESSION *session, WT_SOURCE *ws)
{
	int ret = 0;

	ret = lock_destroy(wtext, session, &ws->lock);

	free(ws->uri);
	free(ws);

	return (ret);
}

/*
 * master_uri_get --
 *	Get the KVS master record for a URI.
 */
static int
master_uri_get(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, const char **valuep)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	return (wtext->metadata_search(wtext, session, uri, valuep));
}

/*
 * master_uri_drop --
 *	Drop the KVS master record for a URI.
 */
static int
master_uri_drop(WT_DATA_SOURCE *wtds, WT_SESSION *session, const char *uri)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	return (wtext->metadata_remove(wtext, session, uri));
}

/*
 * master_uri_rename --
 *	Rename the KVS master record for a URI.
 */
static int
master_uri_rename(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, const char *newuri)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	const char *value;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	value = NULL;

	/* Insert the record under a new name. */
	if ((ret = master_uri_get(wtds, session, uri, &value)) != 0 ||
	    (ret = wtext->metadata_insert(wtext, session, newuri, value)) != 0)
		goto err;

	/*
	 * Remove the original record, and if that fails, attempt to remove
	 * the new record.
	 */
	if ((ret = wtext->metadata_remove(wtext, session, uri)) != 0)
		(void)wtext->metadata_remove(wtext, session, newuri);

err:	free((void *)value);
	return (ret);
}

/*
 * master_uri_set --
 *	Set the KVS master record for a URI.
 */
static int
master_uri_set(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	WT_CONFIG_ITEM a, b;
	WT_EXTENSION_API *wtext;
	int exclusive, ret = 0;
	char value[1024];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	exclusive = 0;
	if ((ret =
	    wtext->config_get(wtext, session, config, "exclusive", &a)) == 0)
		exclusive = a.val != 0;
	else if (ret != WT_NOTFOUND)
		ERET(wtext, session, ret,
		    "exclusive configuration: %s", wtext->strerror(ret));

	/* Get the key/value format strings. */
	if ((ret = wtext->config_get(
	    wtext, session, config, "key_format", &a)) != 0) {
		if (ret == WT_NOTFOUND) {
			a.str = "u";
			a.len = 1;
		} else
			ERET(wtext, session, ret,
			    "key_format configuration: %s",
			    wtext->strerror(ret));
	}
	if ((ret = wtext->config_get(
	    wtext, session, config, "value_format", &b)) != 0) {
		if (ret == WT_NOTFOUND) {
			b.str = "u";
			b.len = 1;
		} else
			ERET(wtext, session, ret,
			    "value_format configuration: %s",
			    wtext->strerror(ret));
	}

	/*
	 * Create a new reference using insert (which fails if the record
	 * already exists).  If that succeeds, we just used up a unique ID,
	 * update the master ID record.
	 */
	(void)snprintf(value, sizeof(value),
	    ",version=(major=%d,minor=%d)"
	    ",key_format=%.*s,value_format=%.*s",
	    KVS_MAJOR, KVS_MINOR, (int)a.len, a.str, (int)b.len, b.str);
	if ((ret = wtext->metadata_insert(wtext, session, uri, value)) == 0)
		return (0);
	if (ret == WT_DUPLICATE_KEY)
		return (exclusive ? EEXIST : 0);
	ERET(wtext, session, ret, "%s: %s", uri, wtext->strerror(ret));
}

/*
 * kvs_session_open_cursor --
 *	WT_SESSION::open_cursor method.
 */
static int
kvs_session_open_cursor(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, WT_CURSOR **new_cursor)
{
	CURSOR *cursor;
	DATA_SOURCE *ds;
	WT_CONFIG_ITEM v;
	WT_CURSOR *wtcursor;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int locked, ret = 0;
	const char *value;

	*new_cursor = NULL;

	cursor = NULL;
	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	ws = NULL;
	locked = 0;
	value = NULL;

	/* Allocate and initialize a cursor. */
	if ((cursor = calloc(1, sizeof(CURSOR))) == NULL)
		return (os_errno());

	cursor->wtcursor.next = kvs_cursor_next;
	cursor->wtcursor.prev = kvs_cursor_prev;
	cursor->wtcursor.reset = kvs_cursor_reset;
	cursor->wtcursor.search = kvs_cursor_search;
	cursor->wtcursor.search_near = kvs_cursor_search_near;
	cursor->wtcursor.insert = kvs_cursor_insert;
	cursor->wtcursor.update = kvs_cursor_update;
	cursor->wtcursor.remove = kvs_cursor_remove;
	cursor->wtcursor.close = kvs_cursor_close;

	cursor->wtext = ds->wtext;

	cursor->record.key = cursor->key;
	if ((cursor->val = malloc(128)) == NULL)
		goto err;
	cursor->val_len = 128;
						/* Parse configuration */
	if ((ret = wtext->config_get(
	    wtext, session, config, "append", &v)) != 0) {
		ESET(wtext, session, ret,
		    "append configuration: %s", wtext->strerror(ret));
		goto err;
	}
	cursor->config_append = v.val != 0;

	if ((ret = wtext->config_get(
	    wtext, session, config, "overwrite", &v)) != 0) {
		ESET(wtext, session, ret,
		    "overwrite configuration: %s", wtext->strerror(ret));
		goto err;
	}
	cursor->config_overwrite = v.val != 0;

	/* Get a locked reference to the WiredTiger source. */
	if ((ret = ws_source_open(wtds, session, config, uri, 0, &ws)) != 0)
		goto err;
	locked = 1;

	/*
	 * Finish initializing the cursor (if the WT_SOURCE structure requires
	 * initialization, we're going to use the cursor as part of that work).
	 */
	cursor->ws = ws;

	/*
	 * If this is the first access to the URI, we have to configure it
	 * using information stored in the master record.
	 */
	if (!ws->configured) {
		if ((ret = master_uri_get(wtds, session, uri, &value)) != 0)
			goto err;

		if ((ret = wtext->config_strget(
		    wtext, session, value, "key_format", &v)) != 0) {
			ESET(wtext, session, ret,
			    "key_format configuration: %s",
			    wtext->strerror(ret));
			goto err;
		}
		ws->config_recno = v.len == 1 && v.str[0] == 'r';

		if ((ret = wtext->config_strget(
		    wtext, session, value, "value_format", &v)) != 0) {
			ESET(wtext, session, ret,
			    "value_format configuration: %s",
			    wtext->strerror(ret));
			goto err;
		}
		ws->config_bitfield =
		    v.len == 2 && isdigit(v.str[0]) && v.str[1] == 't';

		/*
		 * If it's a record-number key, read the last record from the
		 * object and set the allocation record value.
		 */
		if (ws->config_recno) {
			wtcursor = (WT_CURSOR *)cursor;
			if ((ret = kvs_cursor_reset(wtcursor)) != 0)
				goto err;

			if ((ret = kvs_cursor_prev(wtcursor)) == 0)
				ws->append_recno = wtcursor->recno;
			else if (ret != WT_NOTFOUND)
				goto err;

			if ((ret = kvs_cursor_reset(wtcursor)) != 0)
				goto err;
		}

		ws->configured = 1;
	}

	/* Increment the open reference count to pin the URI and unlock it. */
	++ws->ref;
	if ((ret = unlock(wtext, session, &ws->lock)) != 0)
		goto err;

	*new_cursor = (WT_CURSOR *)cursor;

	if (0) {
err:		if (ws != NULL && locked)
			ETRET(unlock(wtext, session, &ws->lock));

		if (cursor != NULL) {
			free(cursor->val);
			free(cursor);
		}
	}
	free((void *)value);
	return (ret);
}

/*
 * kvs_session_create --
 *	WT_SESSION::create method.
 */
static int
kvs_session_create(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/*
	 * Get a locked reference to the WiredTiger source, then immediately
	 * unlock it, we aren't doing anything else.
	 */
	if ((ret = ws_source_open(wtds, session, config, uri, 0, &ws)) != 0)
		return (ret);
	if ((ret = unlock(wtext, session, &ws->lock)) != 0)
		return (ret);

	/*
	 * Create the URI master record if it doesn't already exist.
	 *
	 * If unable to enter a WiredTiger record, leave the KVS store alone.
	 * A subsequent create should do the right thing, we aren't leaving
	 * anything in an inconsistent state.
	 */
	return (master_uri_set(wtds, session, uri, config));
}

/*
 * kvs_session_drop --
 *	WT_SESSION::drop method.
 */
static int
kvs_session_drop(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SOURCE **p, *ws;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/*
	 * Get a locked reference to the data source; hold the global lock,
	 * we are going to change the list of objects for a KVS store.
	 */
	if ((ret = ws_source_open(wtds, session, config, uri, 1, &ws)) != 0)
		return (ret);
	ks = ws->ks;

	/* If there are active references to the object, we're busy. */
	if (ws->ref != 0) {
		ret = EBUSY;
		goto err;
	}

	/*
	 * Close and delete the namespace, then push the change.
	 *
	 * If the close or delete fails, we can return (an error); once delete
	 * succeeds, we have to push through, the object is gone.
	 */
	if ((ret = kvs_close(ws->kvs)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_close: %s: %s", ws->uri, kvs_strerror(ret));
		goto err;
	}
	if ((ret = kvs_delete_namespace(ks->kvs_device, ws->uri)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_delete_namespace: %s: %s", ws->uri, kvs_strerror(ret));
		goto err;
	}

	/*
	 * No turning back, now, accumulate errors as we go.
	 */
	if ((ret = kvs_commit(ks->kvs_device)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_commit: %s", kvs_strerror(ret));

	/* Discard the metadata entry. */
	ETRET(master_uri_drop(wtds, session, uri));

	/*
	 * If we have an error at this point, panic -- there's an inconsistency
	 * in what WiredTiger knows about and the underlying store.
	 */
	if (ret != 0)
		ret = WT_PANIC;

	/*
	 * Remove the entry from the WT_SOURCE list -- it's a singly-linked
	 * list, find the reference to it.
	 */
	for (p = &ks->ws_head; *p != NULL; p = &(*p)->next)
		if (*p == ws) {
			*p = (*p)->next;

			ETRET(ws_source_destroy(wtext, session, ws));
			break;
		}

err:	ETRET(unlock(wtext, session, &ds->global_lock));
	return (ret);
}

/*
 * kvs_session_rename --
 *	WT_SESSION::rename method.
 */
static int
kvs_session_rename(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, const char *newuri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int ret = 0;
	char *copy;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	copy = NULL;

	/*
	 * Get a locked reference to the data source; hold the global lock,
	 * we are going to change the object's name, and we can't allow
	 * other threads walking the list and comparing against the name.
	 */
	if ((ret = ws_source_open(wtds, session, config, uri, 1, &ws)) != 0)
		return (ret);
	ks = ws->ks;

	/* If there are active references to the object, we're busy. */
	if (ws->ref != 0) {
		ret = EBUSY;
		goto err;
	}

	/* Get a copy of the new name, before errors get scary. */
	if ((copy = strdup(newuri)) == NULL) {
		ret = os_errno();
		goto err;
	}

	/*
	 * Rename the KVS namespace and push the change.
	 *
	 * If the rename fails, we can return (an error); once rename succeeds,
	 * we have to push through, the old object is gone.
	 */
	if ((ret = kvs_rename_namespace(ks->kvs_device, uri, newuri)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_rename_namespace: %s: %s", ws->uri, kvs_strerror(ret));
		goto err;
	}

	/*
	 * No turning back, now, accumulate errors as we go.
	 */
	if ((ret = kvs_commit(ws->kvs)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_commit: %s", kvs_strerror(ret));

	/* Update the metadata record. */
	ETRET(master_uri_rename(wtds, session, uri, newuri));

	/* Update the structure name. */
	free(ws->uri);
	ws->uri = copy;
	copy = NULL;

	/*
	 * If we have an error at this point, panic -- there's an inconsistency
	 * in what WiredTiger knows about and the underlying store.
	 */
	if (ret != 0)
		ret = WT_PANIC;

err:	ETRET(unlock(wtext, session, &ds->global_lock));

	free(copy);
	return (ret);
}

/*
 * kvs_session_truncate --
 *	WT_SESSION::truncate method.
 */
static int
kvs_session_truncate(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Get a locked reference to the WiredTiger source. */
	if ((ret = ws_source_open(wtds, session, config, uri, 0, &ws)) != 0)
		return (ret);

	/* If there are active references to the object, we're busy. */
	if (ws->ref != 0) {
		ret = EBUSY;
		goto err;
	}

	/* Truncate the underlying object. */
	if ((ret = kvs_truncate(ws->kvs)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_truncate: %s: %s", ws->uri, kvs_strerror(ret));
		goto err;
	}
	if ((ret = kvs_commit(ws->kvs)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_commit: %s", kvs_strerror(ret));
		goto err;
	}

err:	ETRET(unlock(wtext, session, &ws->lock));
	return (ret);
}

/*
 * kvs_session_verify --
 *	WT_SESSION::verify method.
 */
static int
kvs_session_verify(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;

	(void)uri;
	(void)config;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	ERET(wtext, session, ENOTSUP, "verify: %s", strerror(ENOTSUP));
}

/*
 * kvs_terminate --
 *	Unload the data-source.
 */
static int
kvs_terminate(WT_DATA_SOURCE *wtds, WT_SESSION *session)
{
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int tret, ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	ret = writelock(wtext, session, &ds->global_lock);

	/* Start a flush on any open KVS sources. */
	for (ks = ds->kvs_head; ks != NULL; ks = ks->next)
		if ((tret = kvs_commit(ks->kvs_device)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "kvs_commit: %s", kvs_strerror(tret));

	/* Close and discard all objects. */
	while ((ks = ds->kvs_head) != NULL) {
		while ((ws = ks->ws_head) != NULL) {
			if (ws->ref != 0)
				ESET(wtext, session, WT_ERROR,
				    "%s: has open object %s with %u open "
				    "cursors during close",
				    ks->name, ws->uri, ws->ref);
			if ((tret = kvs_close(ws->kvs)) != 0)
				ESET(wtext, session, WT_ERROR,
				    "kvs_close: %s: %s",
				    ws->uri, kvs_strerror(tret));

			ks->ws_head = ws->next;
			ETRET(ws_source_destroy(wtext, session, ws));
		}

		if ((tret = kvs_close(ks->kvs_device)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "kvs_close: %s: %s", ks->name, kvs_strerror(tret));

		ds->kvs_head = ks->next;
		free(ks->name);
		free(ks);
	}

	ETRET(unlock(wtext, session, &ds->global_lock));
	ETRET(lock_destroy(wtext, NULL, &ds->global_lock));

	free(ds);

	return (ret);
}

/*
 * wiredtiger_extension_init --
 *	Initialize the KVS connector code.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	/*
	 * List of the WT_DATA_SOURCE methods -- it's static so it breaks at
	 * compile-time should the structure changes underneath us.
	 */
	static WT_DATA_SOURCE wtds = {
		kvs_session_create,		/* session.create */
		NULL,				/* No session.compaction */
		kvs_session_drop,		/* session.drop */
		kvs_session_open_cursor,	/* session.open_cursor */
		kvs_session_rename,		/* session.rename */
		NULL,				/* No session.salvage */
		kvs_session_truncate,		/* session.truncate */
		kvs_session_verify,		/* session.verify */
		kvs_terminate			/* termination */
	};
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	(void)config;				/* Unused parameters */

	ds = NULL;
						/* Acquire the extension API */
	wtext = connection->get_extension_api(connection);

						/* Check the library version */
#if KVS_VERSION_MAJOR != 4 || KVS_VERSION_MINOR != 2
	ERET(wtext, NULL, EINVAL,
	    "unsupported KVS library version %d.%d",
	    KVS_VERSION_MAJOR, KVS_VERSION_MINOR);
#endif

	/* Allocate the local data-source structure. */
	if ((ds = calloc(1, sizeof(DATA_SOURCE))) == NULL)
		return (os_errno());
	ds->wtext = wtext;

						/* Configure the global lock */
	if ((ret = lock_init(wtext, NULL, &ds->global_lock)) != 0)
		goto err;

	ds->wtds = wtds;			/* Configure the methods */

	if ((ret = connection->add_data_source(	/* Add the data source */
	    connection, "memrata:", (WT_DATA_SOURCE *)ds, NULL)) != 0) {
		ESET(wtext, NULL, ret,
		    "WT_CONNECTION.add_data_source: %s", wtext->strerror(ret));
		goto err;
	}

	/* Add KVS-specific configuration options. */
	ret = kvs_config_add(connection, wtext);

err:	if (ret != 0)
		free(ds);
	return (ret);
}

/*
 * wiredtiger_extension_terminate --
 *	Shutdown the KVS connector code.
 */
int
wiredtiger_extension_terminate(WT_CONNECTION *connection)
{
	(void)connection;			/* Unused parameters */

	return (0);
}
