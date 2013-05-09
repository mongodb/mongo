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
#undef	ESET
#define	ESET(wtext, session, v, ...) do {				\
	(void)								\
	    wtext->err_printf(wtext, session, "memrata: " __VA_ARGS__);	\
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

typedef struct __kvs_source {
	kvs_t kvs;				/* Underlying KVS reference */
	pthread_rwlock_t lock;			/* KVS lock */

	char *uri;				/* URI */

	u_int open_cursors;			/* Cursor count */

	uint64_t append_recno;			/* Allocation record number */

	struct __kvs_source *next;		/* List of KVS sources */
} KVS_SOURCE;

typedef struct __cursor {
	WT_CURSOR wtcursor;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	KVS_SOURCE *ks;				/* Underlying KVS source */

	struct kvs_record record;		/* Record */
	char   key[KVS_MAX_KEY_LEN];		/* key, value */
	char  *val;
	size_t val_len;

	int	 config_append;			/* config "append" */
	int	 config_bitfield;		/* config "value_format=#t" */
	int	 config_overwrite;		/* config "overwrite" */
	int	 config_recno;			/* config "key_format=r" */
} CURSOR;

typedef struct __data_source {
	WT_DATA_SOURCE wtds;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	pthread_rwlock_t global_lock;		/* Global lock */

	KVS_SOURCE *head;			/* List of KVS sources */
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
 *	Initialize an object's lock.
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
 *	Destroy an object's lock.
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
 *	Release an object's lock.
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
copyin_key(WT_CURSOR *wtcursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	size_t size;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	r = &cursor->record;

	if (cursor->config_recno) {
		if ((ret = wtext->struct_size(wtext, session,
		    &size, "r", wtcursor->recno)) != 0 ||
		    (ret = wtext->struct_pack(wtext, session,
		    r->key, sizeof(r->key), "r", wtcursor->recno)) != 0)
			return (ret);
		r->key_len = size;
	} else {
		/*
		 * XXX
		 * The underlying KVS library data fields aren't const.
		 */
		r->key = (char *)wtcursor->key.data;
		r->key_len = (unsigned long)wtcursor->key.size;
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
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	r = &cursor->record;

	if (cursor->config_recno) {
		if ((ret = wtext->struct_unpack(wtext, session,
		    r->key, sizeof(r->key), "r", &wtcursor->recno)) != 0)
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
	r->val = (char *)wtcursor->value.data;
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

	if (cursor->key == r->key)
		return (0);

	if (r->key_len > sizeof(cursor->key))
		ERET(wtext, session, ERANGE,
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

	kvs = cursor->ks->kvs;
	cursor->record.val = cursor->val;

restart:
	if ((ret = f(kvs, &cursor->record,
	    (unsigned long)0, (unsigned long)cursor->val_len)) != 0) {
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
		    (unsigned long)0, (unsigned long)cursor->val_len)) != 0) {
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

	if ((ret = copyin_key(wtcursor)) != 0)
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
 * kvs_recno_alloc --
 *	Allocate a new record number.
 */
static INLINE int
kvs_recno_alloc(WT_CURSOR *wtcursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ks;

	r = &cursor->record;

	if ((ret = writelock(wtext, session, &ks->lock)) != 0)
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
	if (ks->append_recno == 0) {
		if ((ret = kvs_last(ks->kvs,
		    &cursor->record, (unsigned long)0, (unsigned long)0)) != 0)
			goto err;

		if ((ret = wtext->struct_unpack(wtext, session,
		    r->key, sizeof(r->key), "r", &ks->append_recno)) != 0)
			goto err;
	}

	wtcursor->recno = ++ks->append_recno;

err:	ETRET(unlock(wtext, session, &ks->lock));
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
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ks;

	/* Allocate a new record for append operations. */
	if (cursor->config_append && (ret = kvs_recno_alloc(wtcursor)) != 0)
		return (ret);

	if ((ret = copyin_key(wtcursor)) != 0)
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
		if ((ret = kvs_set(ks->kvs, &cursor->record)) != 0)
			ERET(wtext, session, WT_ERROR,
			    "kvs_set: %s", kvs_strerror(ret));
	} else
		if ((ret = kvs_add(ks->kvs, &cursor->record)) != 0) {
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
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ks;

	if ((ret = copyin_key(wtcursor)) != 0)
		return (ret);
	if ((ret = copyin_val(wtcursor)) != 0)
		return (ret);

	/*
	 * WT_CURSOR::update (update the record if it does exist, fail if it
	 * does not exist), maps to kvs_replace.
	 */
	if ((ret = kvs_replace(ks->kvs, &cursor->record)) != 0)
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
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ks;

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of a single byte of zero.
	 */
	if (cursor->config_bitfield) {
		wtcursor->value.size = 1;
		wtcursor->value.data = "\0";
		return (kvs_cursor_update(wtcursor));
	}

	if ((ret = copyin_key(wtcursor)) != 0)
		return (ret);
	if ((ret = kvs_del(ks->kvs, &cursor->record)) == 0)
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
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ks;

	if ((ret = writelock(wtext, session, &ks->lock)) != 0)
		goto err;
	--ks->open_cursors;
	if ((ret = unlock(wtext, session, &ks->lock)) != 0)
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
kvs_config_add(WT_CONNECTION *connection, WT_EXTENSION_API *wtext)
{
	const KVS_OPTIONS *p;
	int ret = 0;

	/*
	 * KVS options are currently only allowed on session.create, which means
	 * they cannot be modified for each run, the object create configuration
	 * is permanent.
	 */
	for (p = kvs_options; p->name != NULL; ++p)
		if ((ret = connection->configure_method(connection,
		    "session.create", "memrata:",
		    p->name, p->type, p->checks)) != 0)
			ERET(wtext, NULL, ret,
			    "WT_CONNECTION.configure_method: session.create: "
			    "{%s, %s, %s}",
			    p->name, p->type, p->checks, wtext->strerror(ret));
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
		return (ret);
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
		return (ret);
	}
	if ((ret = wtext->config_scan_end(wtext, scan)) != 0) {
		ESET(wtext, session, ret,
		    "WT_EXTENSION_API::config_scan_end: %s",
		    wtext->strerror(ret));
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
drop_data_source(WT_DATA_SOURCE *wtds, WT_SESSION *session, const char *uri)
{
	DATA_SOURCE *ds;
	KVS_SOURCE *p, **ref;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	if ((ret = writelock(wtext, session, &ds->global_lock)) != 0)
		return (ret);

	/* Search our list of objects for a match. */
	for (ref = &ds->head; (p = *ref) != NULL; p = p->next)
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
			ESET(wtext, session, WT_ERROR,
			    "kvs_close: %s: %s", uri, kvs_strerror(ret));
		*ref = p->next;

		ETRET(lock_destroy(wtext, session, &p->lock));
		free(p->uri);
		free(p);
	}

	ETRET(unlock(wtext, session, &ds->global_lock));
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
	struct kvs_config kvs_config;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	kvs_t kvs;
	int flags, ret = 0;
	char **devices, *emsg;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	devices = NULL;
	emsg = NULL;

	/*
	 * Check if the object exists and drop it if we can.  What we really
	 * care about is if it's busy, we don't want to continue in the face
	 * of open cursors.
	 */
	if ((ret = drop_data_source(wtds, session, uri)) != 0)
		return (ret);

	/* Read the configuration. */
	if ((ret = kvs_config_read(
	    wtext, session, config, &devices, &kvs_config, &flags)) != 0)
		goto err;

	/* We require a list of devices underlying the URI. */
	if (devices[0] == NULL) {
		ESET(wtext, session, EINVAL,
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
		ESET(wtext, session, WT_ERROR,
		    "WT_SESSION.create: kvs_open: %s: %s",
		    emsg == NULL ? devices[0] : emsg, kvs_strerror(os_errno()));
		goto err;
	}
	if ((ret = kvs_close(kvs)) != 0) {
		emsg = kvs_create_path_string(devices);
		ESET(wtext, session, WT_ERROR,
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
kvs_session_drop(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	(void)config;				/* Unused parameters */

	/*
	 * Discard the URI from our list.  The KVS object might still exist on
	 * disk, but there's no "drop" operation for KVS, there's no work to do.
	 * Once WiredTiger's meta-data record for the URI is removed, the KVS
	 * object can (must) be re-created, WiredTiger won't allow any other
	 * calls.
	 */
	return (drop_data_source(wtds, session, uri));
}

/*
 * open_data_source --
 *	Open a new data source and insert it into the list.
 */
static int
open_data_source(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	struct kvs_config kvs_config;
	DATA_SOURCE *ds;
	KVS_SOURCE *ks, *p;
	WT_EXTENSION_API *wtext;
	int flags, locked, lockinit, ret = 0;
	char **devices, *emsg;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	ks = NULL;
	devices = NULL;
	emsg = NULL;
	lockinit = locked = 0;

	/*
	 * The first time we open a cursor on an object, allocate an underlying
	 * KVS source object.
	 */
	if ((ks = calloc(1, sizeof(KVS_SOURCE))) == NULL)
		return (os_errno());
	if ((ret = lock_init(wtext, session, &ks->lock)) != 0)
		goto err;
	lockinit = 1;
	if ((ks->uri = strdup(uri)) == NULL)
		goto err;

	/* Read the configuration. */
	if ((ret = kvs_config_read(wtext,
	    session, config, &devices, &kvs_config, &flags)) != 0)
		goto err;

	/* We require a list of devices underlying the URI. */
	if (devices[0] == NULL) {
		ESET(wtext,
		    session, EINVAL, "WT_SESSION.create: no devices specified");
		goto err;
	}

	/*
	 * kvs_open isn't re-entrant: lock things down while we make sure we
	 * don't have more than a single handle at a time.
	 */
	if ((ret = writelock(wtext, session, &ds->global_lock)) != 0)
		goto err;
	locked = 1;

	/*
	 * Check for a match: if we find one, we raced, but we return success,
	 * someone else did the work.
	 */
	for (p = ds->head; p != NULL; p = p->next)
		if (strcmp(p->uri, uri) == 0)
			goto err;

	/* Open the KVS handle. */
	if ((ks->kvs = kvs_open(devices, &kvs_config, flags)) == NULL) {
		emsg = kvs_create_path_string(devices);
		ESET(wtext, session, WT_ERROR,
		    "WT_SESSION.create: kvs_open: %s: %s",
		    emsg == NULL ? devices[0] : emsg, kvs_strerror(os_errno()));
		goto err;
	}

	/* Insert the new entry at the head of the list. */
	ks->next = ds->head;
	ds->head = ks;
	ks = NULL;

err:	if (locked)
		ETRET(unlock(wtext, session, &ds->global_lock));

	if (ks != NULL) {
		if (lockinit)
			ETRET(lock_destroy(wtext, session, &ks->lock));
		free(ks->uri);
		free(ks);
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
kvs_session_open_cursor(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, WT_CURSOR **new_cursor)
{
	CURSOR *cursor;
	DATA_SOURCE *ds;
	KVS_SOURCE *p;
	WT_CONFIG_ITEM v;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	*new_cursor = NULL;

	cursor = NULL;
	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

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

	if ((ret = wtext->config_get(
	    wtext, session, config, "key_format", &v)) != 0) {
		ESET(wtext, session, ret,
		    "key_format configuration: %s", wtext->strerror(ret));
		goto err;
	}
	cursor->config_recno = v.len == 1 && v.str[0] == 'r';

	if ((ret = wtext->config_get(
	    wtext, session, config, "value_format", &v)) != 0) {
		ESET(wtext, session, ret,
		    "value_format configuration: %s", wtext->strerror(ret));
		goto err;
	}
	cursor->config_bitfield =
	    v.len == 2 && isdigit(v.str[0]) && v.str[1] == 't';

	/*
	 * See if the KVS source already exists: if it does, increment the count
	 * of open cursors to pin it, and release the lock.
	 */
	for (;;) {
		if ((ret = writelock(wtext, session, &ds->global_lock)) != 0)
			goto err;
		for (p = ds->head; p != NULL; p = p->next)
			if (strcmp(p->uri, uri) == 0) {
				++p->open_cursors;
				break;
			}
		if ((ret = unlock(wtext, session, &ds->global_lock)) != 0)
			goto err;
		if (p != NULL) {
			cursor->ks = p;
			*new_cursor = (WT_CURSOR *)cursor;
			return (0);
		}

		/* Create the object. */
		if ((ret = open_data_source(wtds, session, uri, config)) != 0)
			goto err;

		/*
		 * We shouldn't loop more than once, but it's theoretically
		 * possible.
		 */
	}

err:	if (cursor != NULL) {
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
kvs_session_rename(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, const char *newname, WT_CONFIG_ARG *config)
{
	(void)newname;				/* Unused parameters */
	(void)config;

	/* Discard the URI from our list. */
	return (drop_data_source(wtds, session, uri));
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
	KVS_SOURCE *p;
	WT_EXTENSION_API *wtext;
	int tret, ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/*
	 * Truncate should work even if the object is not yet opened: if we
	 * don't find it, open it.   We loop because we could theoretically
	 * race with other threads creating/deleting the object.
	 */
	for (;;) {
		if ((ret = writelock(wtext, session, &ds->global_lock)) != 0)
			return (ret);

		/* Search our list of objects for a match. */
		for (p = ds->head; p != NULL; p = p->next)
			if (strcmp(p->uri, uri) == 0)
				break;

		/* If we don't find the object, open it. */
		if (p == NULL) {
			if ((ret =
			    unlock(wtext, session, &ds->global_lock)) != 0)
				return (ret);
			if ((ret =
			    open_data_source(wtds, session, uri, config)) != 0)
				return (ret);
			continue;
		}
		if (p->open_cursors == 0) {
			if ((tret = kvs_truncate(p->kvs)) != 0)
				ESET(wtext, session, WT_ERROR,
				    "kvs_truncate: %s: %s",
				    p->uri, kvs_strerror(tret));
		} else
			ret = EBUSY;
		ETRET(unlock(wtext, session, &ds->global_lock));
		return (ret);
	}
	/* NOTREACHED */
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
	KVS_SOURCE *p;
	WT_EXTENSION_API *wtext;
	int tret, ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	ret = writelock(wtext, session, &ds->global_lock);

	/* Start a flush on any open objects. */
	for (p = ds->head; p != NULL; p = p->next)
		if ((tret = kvs_commit(p->kvs)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "kvs_commit: %s: %s", p->uri, kvs_strerror(tret));

	/* Complain if any of the objects are in use. */
	for (p = ds->head; p != NULL; p = p->next)
		if (p->open_cursors != 0)
			ESET(wtext, session, WT_ERROR,
			    "%s: has open cursors during close", p->uri);

	/* Close and discard the remaining objects. */
	while ((p = ds->head) != NULL) {
		if ((tret = kvs_close(p->kvs)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "kvs_close: %s: %s", p->uri, kvs_strerror(tret));
		ds->head = p->next;

		ETRET(lock_destroy(wtext, session, &p->lock));
		free(p->uri);
		free(p);
	}

	ETRET(unlock(wtext, session, &ds->global_lock));
	ETRET(lock_destroy(wtext, NULL, &ds->global_lock));

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
#if KVS_VERSION_MAJOR != 2 || KVS_VERSION_MINOR != 8
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
	    connection, "memrata:", (WT_DATA_SOURCE *)ds, NULL)) != 0)
		ESET(wtext, NULL, ret,
		    "WT_CONNECTION.add_data_source: %s", wtext->strerror(ret));

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
