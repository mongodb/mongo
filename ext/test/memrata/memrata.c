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
	int		 lockinit;		/* Lock created */

	int	configured;			/* If structure configured */
	u_int	ref;				/* Active reference count */

	uint64_t append_recno;			/* Allocation record number */

	int	 config_recno;			/* config "key_format=r" */
	int	 config_bitfield;		/* config "value_format=#t" */

	/*
	 * Each WiredTiger object has a "primary" namespace in a KVS store plus
	 * a "cache" namespace, which has not-yet-resolved updates.  There's a
	 * dirty flag so we can ignore the cache until it's used.
	 */
	kvs_t kvs;				/* Underlying KVS object */
	kvs_t kvscache;				/* Underlying KVS cache */
	int   kvscache_inuse;

	struct __kvs_source *ks;		/* Underlying KVS source */
	struct __wt_source *next;		/* List of WiredTiger objects */
} WT_SOURCE;

typedef struct __kvs_source {
	char *name;				/* Unique name */

	kvs_t kvs_device;			/* Underlying KVS store */

	/*
	 * Each underlying KVS source has a txn namespace which has the list of
	 * active transactions with their committed or aborted state as a value.
	 */
#define	TXN_ABORTED	1
#define	TXN_COMMITTED	2
#define	TXN_UNRESOLVED	3
	kvs_t kvstxn;				/* Underlying KVS txn store */

	struct __wt_source *ws_head;		/* List of WiredTiger sources */

	struct __kvs_source *next;		/* List of KVS sources */
} KVS_SOURCE;

typedef struct __data_source {
	WT_DATA_SOURCE wtds;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	pthread_rwlock_t global_lock;		/* Global lock */

	KVS_SOURCE *kvs_head;			/* List of KVS sources */
} DATA_SOURCE;

/*
 * Values in the cache store are marshalled/unmarshalled to/from the store,
 * using a simple encoding:
 *	{N records: 4B}
 *	{record#1 TxnID: 8B}
 *	{record#1 remove tombstone: 1B}
 *	{record#1 data length: 4B}
 *	{record#1 data}
 *	...
 *
 * Each KVS cursor potentially has a single set of these values.
 */
typedef struct __cache_record {
	uint8_t	*v;				/* Value */
	uint32_t len;				/* Value length */
	uint64_t txnid;				/* Transaction ID */
#define	REMOVE_TOMBSTONE	'R'
	int	 remove;			/* 1/0 remove flag */
} CACHE_RECORD;

typedef struct __cursor {
	WT_CURSOR wtcursor;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	WT_SOURCE *ws;				/* WiredTiger source */

	struct kvs_record record;		/* Record */
	uint8_t  key[KVS_MAX_KEY_LEN];		/* key, value */
	uint8_t *v;
	size_t   len;
	size_t	 mem_len;

	struct {
		uint8_t *v;			/* Temporary buffers */
		size_t   len;
		size_t   mem_len;
	} t1, t2, t3;

	int	 config_append;			/* config "append" */
	int	 config_overwrite;		/* config "overwrite" */

	CACHE_RECORD	*cache;			/* unmarshalled cache records */
	uint32_t	 cache_entries;		/* cache records */
	uint32_t	 cache_slots;		/* cache total record slots */
} CURSOR;

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
static inline int
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
static inline int
unlock(WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_unlock(lockp)) != 0)
		ERET(wtext, session, WT_PANIC, "unlock: %s", strerror(ret));
	return (0);
}

/*
 * txn_state_set --
 *	Resolve a transaction.
 */
static int
txn_state_set(WT_CURSOR *wtcursor, int commit)
{
	struct kvs_record txn;
	CURSOR *cursor;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	uint64_t txnid;
	uint8_t val;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ws->ks;

	/* Get the transaction ID. */
	txnid = wtext->transaction_id(wtext, session);

	val = commit ? TXN_COMMITTED : TXN_ABORTED;

	/* Update the store -- commits must be permanent, flush the device. */
	txn.key = &txnid;
	txn.key_len = sizeof(txnid);
	txn.val = &val;
	txn.val_len = 1;
	if ((ret = kvs_set(ks->kvstxn, &txn)) != 0 ||
	    (commit && (ret = kvs_commit(ks->kvs_device)) != 0))
		ERET(wtext, session,
		    WT_ERROR, "kvs_commit: %s", kvs_strerror(ret));
	return (0);
}

/*
 * txn_state --
 *	Return a transaction's state.
 */
static int
txn_state(WT_CURSOR *wtcursor, uint64_t txnid)
{
	struct kvs_record txn;
	CURSOR *cursor;
	KVS_SOURCE *ks;
	uint8_t val_buf[16];

	cursor = (CURSOR *)wtcursor;
	ks = cursor->ws->ks;

	txn.key = &txnid;
	txn.key_len = sizeof(txnid);
	txn.val = val_buf;
	txn.val_len = sizeof(val_buf);

	if (kvs_get(ks->kvstxn, &txn, 0UL, (unsigned long)sizeof(val_buf)) == 0)
		return (val_buf[0]);
	return (TXN_UNRESOLVED);
}

/*
 * cache_value_append --
 *	Append the current WiredTiger cursor's value to a cache record.
 */
static int
cache_value_append(WT_CURSOR *wtcursor, int remove_op)
{
	struct kvs_record *r;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	uint64_t txnid;
	size_t len;
	uint32_t slots;
	uint8_t *p;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	r = &cursor->record;

	/*
	 * The format of a cache update is the value, 8B of transaction ID
	 * and 1B of remove tombstone.  Grow the value buffer as necessary,
	 * then append the WiredTiger cursor's information.
	 */
	len = cursor->len + sizeof(uint32_t);		/* slots */
	if (!remove_op)
		len += wtcursor->value.size;		/* data */
	len += sizeof(uint64_t) + 1;			/* txn ID, remove */
	if (len > cursor->mem_len) {
		if ((p = realloc(cursor->v, len + 64)) == NULL)
			return (os_errno());
		cursor->v = p;
		cursor->mem_len = len + 64;
	}

	/* Get the transaction ID. */
	txnid = wtext->transaction_id(wtext, session);

	/* Update the number of records in this value. */
	if (cursor->len == 0) {
		slots = 1;
		cursor->len = sizeof(uint32_t);
	} else {
		memcpy(&slots, cursor->v, sizeof(uint32_t));
		++slots;
	}
	memcpy(cursor->v, &slots, sizeof(uint32_t));

	/*
	 * Copy the WiredTiger cursor's data into place: txn ID, remove
	 * tombstone, data length, data.
	 */
	p = cursor->v + cursor->len;
	memcpy(p, &txnid, sizeof(uint64_t));
	p += sizeof(uint64_t);
	if (remove_op)
		*p++ = REMOVE_TOMBSTONE;
	else {
		*p++ = '\0';
		memcpy(p, &wtcursor->value.size, sizeof(uint32_t));
		p += sizeof(uint32_t);
		memcpy(p, wtcursor->value.data, wtcursor->value.size);
		p += wtcursor->value.size;
	}
	cursor->len = (size_t)(p - cursor->v);

	/* Update the underlying KVS record. */
	r->val = cursor->v;
	r->val_len = cursor->len;

	return (0);
}

/*
 * cache_value_unmarshall --
 *	Unmarshall a cache value into a set of records.
 */
static int
cache_value_unmarshall(WT_CURSOR *wtcursor)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	uint32_t entries, i;
	uint8_t *p;
	int ret = 0;

	cursor = (CURSOR *)wtcursor;

	/* If we don't have enough record slots, allocate some more. */
	memcpy(&entries, cursor->v, sizeof(uint32_t));
	if (entries > cursor->cache_slots) {
		if ((p = realloc(cursor->cache,
		    (entries + 20) * sizeof(cursor->cache[0]))) == NULL)
			return (os_errno());

		cursor->cache = (CACHE_RECORD *)p;
		cursor->cache_slots = entries + 20;
	}

	/* Walk the value, splitting it up into records. */
	p = cursor->v + sizeof(uint32_t);
	for (i = 0, cp = cursor->cache; i < entries; ++i, ++cp) {
		memcpy(&cp->txnid, p, sizeof(uint64_t));
		p += sizeof(uint64_t);
		cp->remove = *p++ == REMOVE_TOMBSTONE ? 1 : 0;
		if (!cp->remove) {
			memcpy(&cp->len, p, sizeof(uint32_t));
			p += sizeof(uint32_t);
			cp->v = p;
			p += cp->len;
		}
	}
	cursor->cache_entries = entries;

	return (ret);
}

/*
 * cache_value_visible --
 *	Return if a record in the cache is visible to this transaction.
 */
static int
cache_value_visible(WT_CURSOR *wtcursor, CACHE_RECORD **cpp)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	u_int i;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	/*
	 * We need to return the "best" reference, the largest txn ID visible
	 * to us; the cache entries are in reverse order, walk them that way
	 * to ensure we get the best match.
	 */
	cp = cursor->cache + cursor->cache_entries;
	for (i = 0; i < cursor->cache_entries; ++i) {
		--cp;

		/*
		 * XXX
		 * WiredTiger resets updated entry transaction IDs to an aborted
		 * state on rollback; to do that here would require tracking the
		 * updated entries for a transaction or scanning the cache for
		 * updates made for the transaction during rollback, expensive
		 * stuff.  Instead, check if the transaction has been aborted
		 * before calling the underlying WiredTiger visibility function.
		 */
		if (txn_state(wtcursor, cp->txnid) != TXN_ABORTED &&
		    wtext->transaction_visible(wtext, session, cp->txnid)) {
			if (cpp != NULL)
				*cpp = cp;
			return (1);
		}
	}
	return (0);
}

/*
 * cache_value_update_check --
 *	Return if an update can proceed.
 */
static int
cache_value_update_check(WT_CURSOR *wtcursor)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	u_int i;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	/* Only interesting for snapshot isolation. */
	if (!wtext->transaction_snapshot_isolation(wtext, session))
		return (0);

	/*
	 * If there's an entry that's not visible and hasn't been aborted,
	 * return a deadlock.
	 */
	cp = cursor->cache + cursor->cache_entries;
	for (i = 0; i < cursor->cache_entries; ++i) {
		--cp;

		/*
		 * XXX
		 * WiredTiger resets updated entry transaction IDs to an aborted
		 * state on rollback; to do that here would require tracking the
		 * updated entries for a transaction or scanning the cache for
		 * updates made for the transaction during rollback, expensive
		 * stuff.  Instead, check if the transaction has been aborted
		 * before calling the underlying WiredTiger visibility function.
		 */
		if (txn_state(wtcursor, cp->txnid) != TXN_ABORTED &&
		    !wtext->transaction_visible(wtext, session, cp->txnid))
			return (WT_DEADLOCK);
	}
	return (0);
}

/*
 * copyin_key --
 *	Copy a WT_CURSOR key to a struct kvs_record key.
 */
static inline int
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
		/* I'm not sure this test is necessary, but it's cheap. */
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
static inline int
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
 * copyout_val --
 *	Copy a kvs store's struct kvs_record value to a WT_CURSOR value.
 */
static inline int
copyout_val(WT_CURSOR *wtcursor, CACHE_RECORD *cp)
{
	struct kvs_record *r;
	CURSOR *cursor;

	cursor = (CURSOR *)wtcursor;
	r = &cursor->record;

	if (cp == NULL) {
		wtcursor->value.data = r->val;
		wtcursor->value.size = (uint32_t)r->val_len;
	} else {
		wtcursor->value.data = cp->v;
		wtcursor->value.size = cp->len;
	}
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
static inline int
kvs_call(WT_CURSOR *wtcursor, const char *fname, kvs_t kvs,
    int (*f)(kvs_t, struct kvs_record *, unsigned long, unsigned long))
{
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;
	char *p;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	cursor->record.val = cursor->v;

restart:
	if ((ret = f(kvs, &cursor->record,
	    0UL, (unsigned long)cursor->mem_len)) != 0) {
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
		if (cursor->mem_len >= cursor->record.val_len) {
			cursor->len = cursor->record.val_len;
			return (0);
		}

		/* Grow the value buffer. */
		if ((p =
		    realloc(cursor->v, cursor->record.val_len + 32)) == NULL)
			return (os_errno());
		cursor->v = cursor->record.val = p;
		cursor->mem_len = cursor->record.val_len + 32;

		if ((ret = kvs_get(kvs, &cursor->record,
		    0UL, (unsigned long)cursor->mem_len)) != 0) {
			if (ret == KVS_E_KEY_NOT_FOUND)
				goto restart;
			ERET(wtext, session,
			    WT_ERROR, "kvs_get: %s", kvs_strerror(ret));
		}
	}
	/* NOTREACHED */
}

/*
 * nextprev --
 *	Cursor next/prev.
 */
static int
nextprev(WT_CURSOR *wtcursor, const char *fname,
    int (*f)(kvs_t, struct kvs_record *, unsigned long, unsigned long))
{
	struct kvs_record *r;
	CACHE_RECORD *cp;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_ITEM a, b;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int cache_ret, cmp, ret = 0;
	void *p;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	ws = cursor->ws;
	wtext = cursor->wtext;

	r = &cursor->record;

	/*
	 * If the underlying KVS function is going to return a key, we have to
	 * be sure the key buffer is large enough into which to copy any key,
	 * that is, the key can't reference the WT_CURSOR key.  If the previous
	 * call was set up so the key referenced the WT_CURSOR key, copy it to
	 * the CURSOR key buffer.
	 */
	if (r->key != cursor->key) {
		if (r->key_len > sizeof(cursor->key))
			ERET(wtext, session, ERANGE,
			    "key too large, maximum is %" PRIuMAX,
			    (uintmax_t)sizeof(cursor->key));

		memcpy(cursor->key, r->key, r->key_len);
		r->key = cursor->key;
	}

	/*
	 * If the cache isn't yet in use, it's a simpler problem, just check
	 * the store.  We don't care if we race, we're not guaranteeing any
	 * special behavior with respect to phantoms.
	 */
	if (ws->kvscache_inuse == 0) {
		cache_ret = WT_NOTFOUND;
		goto cache_clean;
	}

	/*
	 * The next/prev key/value pair might be in the cache, which means we
	 * are making two calls and returning the best choice.  As each call
	 * overwrites both key and value, we have to have a copy of the key
	 * for the second call plus the returned key and value from the first
	 * call.   That's why each cursor has 3 temporary buffers.
	 *
	 * First, copy the key.
	 */
	if (cursor->t1.mem_len < r->key_len) {
		if ((p = realloc(cursor->t1.v, r->key_len)) == NULL)
			return (os_errno());
		cursor->t1.v = p;
		cursor->t1.mem_len = r->key_len;
	}
	memcpy(cursor->t1.v, r->key, r->key_len);
	cursor->t1.len = r->key_len;

	/*
	 * Move through the cache until we either find a record with a visible
	 * entry, or we reach the end/beginning.
	 */
	for (;;) {
		if ((ret = kvs_call(wtcursor, fname, ws->kvscache, f)) != 0)
			break;
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			return (ret);

		/*
		 * If there's no visible entry, or the visible entry is a
		 * deleted item, skip it.
		 */
		if (!cache_value_visible(wtcursor, &cp))
			continue;
		if (cp->remove)
			continue;

		/*
		 * Copy the cache key/value pair, we will need them if we are
		 * to return the cache's entry.
		 */
		if (cursor->t2.mem_len < r->key_len) {
			if ((p = realloc(cursor->t2.v, r->key_len)) == NULL)
				return (os_errno());
			cursor->t2.v = p;
			cursor->t2.mem_len = r->key_len;
		}
		memcpy(cursor->t2.v, r->key, r->key_len);
		cursor->t2.len = r->key_len;

		if (cursor->t3.mem_len < cp->len) {
			if ((p = realloc(cursor->t3.v, cp->len)) == NULL)
				return (os_errno());
			cursor->t3.v = p;
			cursor->t3.mem_len = cp->len;
		}
		memcpy(cursor->t3.v, cp->v, cp->len);
		cursor->t3.len = cp->len;

		break;
	}
	if (ret != 0 && ret != WT_NOTFOUND)
		return (ret);
	cache_ret = ret;

	/* Copy the original key back into place. */
	memcpy(r->key, cursor->t1.v, cursor->t1.len);
	r->key_len = cursor->t1.len;

cache_clean:
	/* Get the next/prev entry from the store. */
	if ((ret = kvs_call(wtcursor, fname, ws->kvs, f)) == 0) {
		if ((ret = copyout_val(wtcursor, NULL)) != 0)
			return (ret);
	} else if (ret != WT_NOTFOUND)
		return (ret);

	/* If no entries in either the cache or the primary, we're done. */
	if (cache_ret == WT_NOTFOUND && ret == WT_NOTFOUND)
		return (WT_NOTFOUND);

	/*
	 * If both the cache and the primary had entries, decide which is a
	 * better choice and pretend we didn't find the other one.
	 */
	if (cache_ret == 0 && ret == 0) {
		a.data = cursor->v;		/* a is the primary */
		a.size = (uint32_t)cursor->len;
		b.data = cursor->t3.v;		/* b is the cache */
		b.size = (uint32_t)cursor->t3.len;
		if ((ret = wtext->collate(wtext, session, &a, &b, &cmp)) != 0)
			return (ret);
		if (f == kvs_next) {
			if (cmp < 0)
				cache_ret = WT_NOTFOUND;
			else
				ret = WT_NOTFOUND;
		} else {
			if (cmp < 0)
				ret = WT_NOTFOUND;
			else
				cache_ret = WT_NOTFOUND;
		}
	}

	/* If no entry in the primary, copy the cache's entry into place. */
	if (cache_ret == 0 && ret == WT_NOTFOUND) {
		memcpy(r->key, cursor->t2.v, cursor->t2.len);
		r->key_len = cursor->t2.len;

		memcpy(cursor->v, cursor->t3.v, cursor->t3.len);
		cursor->len = cursor->t3.len;
	}

	/* Copy out the chosen key/value pair. */
	if ((ret = copyout_key(wtcursor)) != 0)
		return (ret);
	if ((ret = copyout_val(wtcursor, NULL)) != 0)
		return (ret);
	return (0);
}

/*
 * kvs_cursor_next --
 *	WT_CURSOR::next method.
 */
static int
kvs_cursor_next(WT_CURSOR *wtcursor)
{
	return (nextprev(wtcursor, "kvs_next", kvs_next));
}

/*
 * kvs_cursor_prev --
 *	WT_CURSOR::prev method.
 */
static int
kvs_cursor_prev(WT_CURSOR *wtcursor)
{
	return (nextprev(wtcursor, "kvs_prev", kvs_prev));
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
	CACHE_RECORD *cp;
	CURSOR *cursor;
	WT_SOURCE *ws;
	int ret = 0;

	cursor = (CURSOR *)wtcursor;
	ws = cursor->ws;

	/* Copy in the WiredTiger cursor's key. */
	if ((ret = copyin_key(wtcursor, 0)) != 0)
		return (ret);

	/*
	 * Check for an entry in the cache.  If we find one, unmarshall it
	 * and check for a visible entry we can return.
	 */
	if ((ret = kvs_call(wtcursor, "kvs_get", ws->kvscache, kvs_get)) == 0) {
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			return (ret);
		if (cache_value_visible(wtcursor, &cp)) {
			if ((ret = copyout_val(wtcursor, cp)) != 0)
				return (ret);
			return (cp->remove ? WT_NOTFOUND : 0);
		}
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	/* Check for an entry in the primary store. */
	if ((ret = kvs_call(wtcursor, "kvs_get", ws->kvs, kvs_get)) != 0)
		return (ret);
	if ((ret = copyout_val(wtcursor, NULL)) != 0)
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
	CACHE_RECORD *cp;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;

	/* Get the WiredTiger cursor's key. */
	if ((ret = copyin_key(wtcursor, 1)) != 0)
		return (ret);

	/* Clear the value, assume we're adding the first cache entry. */
	cursor->len = 0;

	/* Updates are read-modify-writes, lock the underlying cache. */
	if ((ret = writelock(wtext, session, &ws->lock)) != 0)
		return (ret);

	/* Read the record from the cache store. */
	switch (ret = kvs_call(wtcursor, "kvs_get", ws->kvscache, kvs_get)) {
	case 0:
		/* Crack the record. */
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			goto err;

		/* Check if the update can proceed. */
		if ((ret = cache_value_update_check(wtcursor)) != 0)
			goto err;

		if (cursor->config_overwrite)
			break;

		/*
		 * If overwrite is false, a visible entry (that's not a removed
		 * entry), is an error.  We're done checking if there is a
		 * visible entry in the cache, otherwise repeat the check on the
		 * primary store.
		 */
		if (cache_value_visible(wtcursor, &cp)) {
			if (cp->remove)
				break;

			ret = WT_DUPLICATE_KEY;
			goto err;
		}
		/* FALLTHROUGH */
	case WT_NOTFOUND:
		if (cursor->config_overwrite)
			break;

		/* If overwrite is false, an entry is an error. */
		if ((ret = kvs_call(
		    wtcursor, "kvs_get", ws->kvs, kvs_get)) != WT_NOTFOUND) {
			if (ret == 0)
				ret = WT_DUPLICATE_KEY;
			goto err;
		}
		ret = 0;
		break;
	default:
		goto err;
	}

	/*
	 * Create a new cache value based on the current cache record plus the
	 * WiredTiger cursor's value.
	 */
	if ((ret = cache_value_append(wtcursor, 0)) != 0)
		goto err;

	/* Push the record into the cache. */
	if ((ret = kvs_set(ws->kvscache, &cursor->record)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_set: %s", kvs_strerror(ret));
	ws->kvscache_inuse = 1;

#if 0
	/*
	 * WT_CURSOR::insert with overwrite set (create the record if it does
	 * not exist, update the record if it does exist), maps to kvs_set.
	 *
	 * WT_CURSOR::insert without overwrite set (create the record if it
	 * does not exist, fail if it does exist), maps to kvs_add.
	 */
	if (cursor->config_overwrite) {
		if ((ret = kvs_set(ws->kvs, &cursor->record)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "kvs_set: %s", kvs_strerror(ret));
	} else
		if ((ret = kvs_add(ws->kvs, &cursor->record)) != 0) {
			if (ret == KVS_E_KEY_EXISTS)
				ret = WT_DUPLICATE_KEY;
			else
				ESET(wtext, session, WT_ERROR,
				    "kvs_add: %s", kvs_strerror(ret));
		}
#endif

	/* Discard the lock. */
err:	ETRET(unlock(wtext, session, &ws->lock));

	return (ret);
}

/*
 * update --
 *	Update or remove an entry.
 */
static int
update(WT_CURSOR *wtcursor, int remove_op)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;

	/* Get the WiredTiger cursor's key. */
	if ((ret = copyin_key(wtcursor, 0)) != 0)
		return (ret);

	/* Clear the value, assume we're adding the first cache entry. */
	cursor->len = 0;

	/* Updates are read-modify-writes, lock the underlying cache. */
	if ((ret = writelock(wtext, session, &ws->lock)) != 0)
		return (ret);

	/* Read the record from the cache store. */
	switch (ret = kvs_call(wtcursor, "kvs_get", ws->kvscache, kvs_get)) {
	case 0:
		/* Crack the record. */
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			goto err;

		/* Check if the update can proceed. */
		if ((ret = cache_value_update_check(wtcursor)) != 0)
			goto err;

		if (cursor->config_overwrite)
			break;

		/*
		 * If overwrite is false, no entry (or a removed entry), is an
		 * error.   We're done checking if there is a visible entry in
		 * the cache, otherwise repeat the check on the primary store.
		 */
		if (cache_value_visible(wtcursor, &cp)) {
			if (!cp->remove)
				break;

			ret = WT_NOTFOUND;
			goto err;
		}
		/* FALLTHROUGH */
	case WT_NOTFOUND:
		if (cursor->config_overwrite)
			break;

		/* If overwrite is false, no entry is an error. */
		if ((ret = kvs_call(
		    wtcursor, "kvs_get", ws->kvs, kvs_get)) != 0)
			goto err;
		break;
	default:
		goto err;
	}

	/*
	 * Create a new cache value based on the current cache record plus the
	 * WiredTiger cursor's value.
	 */
	if ((ret = cache_value_append(wtcursor, remove_op)) != 0)
		goto err;

	/* Push the record into the cache. */
	if ((ret = kvs_set(ws->kvscache, &cursor->record)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_set: %s", kvs_strerror(ret));
	ws->kvscache_inuse = 1;

#if 0
	/*
	 * WT_CURSOR::update without overwrite set (update the record if it
	 * does exist, fail if it does not exist), maps to kvs_replace.  We
	 * only implement the overwrite not-set path here: if overwrite was
	 * set, we pointed the cursor update function at the insert function
	 * when it was configured, because they're identical.
	 */
	if ((ret = kvs_replace(ws->kvs, &cursor->record)) != 0)
		ERET(wtext,
		    session, WT_ERROR, "kvs_replace: %s", kvs_strerror(ret));

	return (0);

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of a single byte of zero.
	 */
	if (ws->config_bitfield) {
		wtcursor->value.size = 1;
		wtcursor->value.data = "\0";
		return (wtcursor->update(wtcursor));
	}

	if ((ret = copyin_key(wtcursor, 0)) != 0)
		return (ret);
	if ((ret = kvs_del(ws->kvs, &cursor->record)) == 0)
		return (0);
	if (ret == KVS_E_KEY_NOT_FOUND)
		return (cursor->config_overwrite ? 0 : WT_NOTFOUND);
	ERET(wtext, session, WT_ERROR, "kvs_del: %s", kvs_strerror(ret));
#endif

	/* Discard the lock. */
err:	ETRET(unlock(wtext, session, &ws->lock));

	return (ret);
}

/*
 * kvs_cursor_update --
 *	WT_CURSOR::update method.
 */
static int
kvs_cursor_update(WT_CURSOR *wtcursor)
{
	return (update(wtcursor, 0));
}

/*
 * kvs_cursor_remove --
 *	WT_CURSOR::remove method.
 */
static int
kvs_cursor_remove(WT_CURSOR *wtcursor)
{
	return (update(wtcursor, 1));
}

/*
 * kvs_cursor_commit --
 *	WT_CURSOR::commit method.
 */
static int
kvs_cursor_commit(WT_CURSOR *wtcursor)
{
	return (txn_state_set(wtcursor, 1));
}

/*
 * kvs_cursor_rollback --
 *	WT_CURSOR::rollback method.
 */
static int
kvs_cursor_rollback(WT_CURSOR *wtcursor)
{
	return (txn_state_set(wtcursor, 0));
}

/*
 * cursor_destroy --
 *	Free a cursor and it's memory.
 */
static void
cursor_destroy(CURSOR *cursor)
{
	free(cursor->v);
	free(cursor->t1.v);
	free(cursor->t2.v);
	free(cursor->t3.v);
	free(cursor->cache);
	free(cursor);
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

	if ((ret = writelock(wtext, session, &ws->lock)) == 0) {
		--ws->ref;
		ret = unlock(wtext, session, &ws->lock);
	}
	cursor_destroy(cursor);

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
 * kvs_source_close --
 *	Kill a KVS_SOURCE structure.
 */
static int
kvs_source_close(WT_EXTENSION_API *wtext, WT_SESSION *session, KVS_SOURCE *ks)
{
	int ret = 0;

	if (ks->kvstxn != NULL && (ret = kvs_close(ks->kvstxn)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_close: %s.txn: %s", ks->name, kvs_strerror(ret));
	ks->kvstxn = NULL;

	if (ks->kvs_device != NULL && (ret = kvs_close(ks->kvs_device)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_close: %s: %s", ks->name, kvs_strerror(ret));
	ks->kvs_device = NULL;

	free(ks->name);
	free(ks);

	return (ret);
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

	/* Open the underlying KVS store (creating it if necessary). */
	ks->kvs_device =
	    kvs_open(device_list, &kvs_config, flags | KVS_O_CREATE);
	if (ks->kvs_device == NULL) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_open: %s: %s", ks->name, kvs_strerror(os_errno()));
		goto err;
	}

	/*
	 * Open the global txn namespace.
	 *
	 * XXX
	 * This is not quite correct: if there are multiple KVS devices, we'd
	 * only want to open one of the transaction namespaces, and subsequent
	 * KVS device opens would reference the same transaction name space.
	 *
	 * XXX
	 * This is where we'll handle recovery of the global txn namespace, we
	 * have to review any list of committed transactions in the txn store
	 * and update the primary as necessary.
	 */
	if ((ks->kvstxn =
	    kvs_open_namespace(ks->kvs_device, "txn", KVS_O_CREATE)) == NULL) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_open_namespace: %s.txn: %s",
		    ks->name, kvs_strerror(os_errno()));
		goto err;
	}

	/* Push the change. */
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

	if (ks != NULL)
		ETRET(kvs_source_close(wtext, session, ks));

	if (device_list != NULL) {
		for (p = device_list; *p != NULL; ++p)
			free(*p);
		free(device_list);
	}
	free(devices);
	return (ret);
}

/*
 * ws_source_name --
 *	Build a namespace name.
 */
static inline int
ws_source_name(
    const char *uri, const char *suffix, const char **pp, char **bufp)
{
	size_t len;
	char *buf;

	*bufp = NULL;

	/* Create the store's name: the uri with an optional trailing suffix. */
	if (suffix == NULL)
		*pp = uri;
	else {
		len = strlen(uri) + strlen(".") + strlen(suffix) + 5;
		if ((buf = malloc(len)) == NULL)
			return (os_errno());
		(void)snprintf(buf, len, "%s.%s", uri, suffix);
		*pp = buf;
	}
	return (0);
}

/*
 * ws_source_drop_namespace --
 *	Drop a namespace.
 */
static int
ws_source_drop_namespace(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, const char *suffix, kvs_t kvs_device)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	const char *p;
	char *buf;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	buf = NULL;

	/* Drop the underlying KVS namespace. */
	if ((ret = ws_source_name(uri, suffix, &p, &buf)) != 0)
		return (ret);
	if ((ret = kvs_delete_namespace(kvs_device, p)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_delete_namespace: %s: %s", p, kvs_strerror(ret));

	free(buf);
	return (ret);
}

/*
 * ws_source_rename_namespace --
 *	Rename a namespace.
 */
static int
ws_source_rename_namespace(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, const char *newuri, const char *suffix, kvs_t kvs_device)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	const char *p, *pnew;
	char *buf, *bufnew;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	buf = bufnew = NULL;

	/* Rename the underlying KVS namespace. */
	ret = ws_source_name(uri, suffix, &p, &buf);
	if (ret == 0)
		ret = ws_source_name(newuri, suffix, &pnew, &bufnew);
	if (ret == 0 && (ret = kvs_rename_namespace(kvs_device, p, pnew)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_rename_namespace: %s: %s", p, kvs_strerror(ret));

	free(buf);
	free(bufnew);
	return (ret);
}

/*
 * ws_source_close --
 *	Kill a WT_SOURCE structure.
 */
static int
ws_source_close(WT_EXTENSION_API *wtext, WT_SESSION *session, WT_SOURCE *ws)
{
	int ret = 0;

	if (ws->ref != 0)
		ESET(wtext, session, WT_ERROR,
		    "%s: open object with %u open cursors being closed",
		    ws->uri, ws->ref);

	if (ws->kvs != NULL && (ret = kvs_close(ws->kvs)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_close: %s: %s", ws->uri, kvs_strerror(ret));
	ws->kvs = NULL;
	if (ws->kvscache != NULL && (ret = kvs_close(ws->kvscache)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_close: %s.cache: %s", ws->uri, kvs_strerror(ret));
	ws->kvscache = NULL;

	if (ws->lockinit)
		ETRET(lock_destroy(wtext, session, &ws->lock));

	free(ws->uri);
	free(ws);

	return (ret);
}

/*
 * ws_source_open_namespace --
 *	Open a namespace.
 */
static int
ws_source_open_namespace(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, const char *suffix, kvs_t kvs_device, kvs_t *kvsp)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	kvs_t kvs;
	int ret = 0;
	const char *p;
	char *buf;

	*kvsp = NULL;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	buf = NULL;

	/* Open the underlying KVS namespace (creating it if necessary). */
	if ((ret = ws_source_name(uri, suffix, &p, &buf)) != 0)
		return (ret);
	if ((kvs = kvs_open_namespace(kvs_device, p, KVS_O_CREATE)) == NULL)
		ESET(wtext, session, WT_ERROR,
		    "kvs_open_namespace: %s: %s", p, kvs_strerror(os_errno()));
	*kvsp = kvs;

	free(buf);
	return (ret);
}

#define	WS_SOURCE_OPEN_BUSY	0x01		/* Fail if source busy */
#define	WS_SOURCE_OPEN_GLOBAL	0x02		/* Keep the global lock */

/*
 * ws_source_open --
 *	Return a locked WiredTiger source, allocating and opening if it doesn't
 * already exist.
 */
static int
ws_source_open(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, u_int flags, WT_SOURCE **refp)
{
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int ret = 0;

	*refp = NULL;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	ws = NULL;

	/* Get the underlying, locked, KVS source. */
	if ((ret = kvs_source_open(wtds, session, config, &ks)) != 0)
		return (ret);

	/*
	 * Check for a match: if we find one, optionally trade the global lock
	 * for the object, optionally check if the object is busy, and return.
	 */
	for (ws = ks->ws_head; ws != NULL; ws = ws->next)
		if (strcmp(ws->uri, uri) == 0) {
			/* Check to see if the object is busy. */
			if (ws->ref != 0 && (flags & WS_SOURCE_OPEN_BUSY)) {
				ret = EBUSY;
				ETRET(unlock(wtext, session, &ds->global_lock));
				return (ret);
			}
			/* Swap the global lock for an object lock. */
			if (!(flags & WS_SOURCE_OPEN_GLOBAL)) {
				ret = writelock(wtext, session, &ws->lock);
				ETRET(unlock(wtext, session, &ds->global_lock));
				if (ret != 0)
					return (ret);
			}
			*refp = ws;
			return (0);
		}

	/* Allocate and initialize a new underlying WiredTiger source object. */
	if ((ws = calloc(1, sizeof(*ws))) == NULL ||
	    (ws->uri = strdup(uri)) == NULL) {
		ret = os_errno();
		goto err;
	}
	if ((ret = lock_init(wtext, session, &ws->lock)) != 0)
		goto err;
	ws->lockinit = 1;
	ws->ks = ks;

	/*
	 * Open the underlying KVS namespaces (optionally creating them), then
	 * push the change.
	 *
	 * The naming scheme is simple: the URI names the primary store, and the
	 * URI with a trailing suffix names the associated caching store.
	 */
	if ((ret = ws_source_open_namespace(
	    wtds, session, uri, NULL, ks->kvs_device, &ws->kvs)) != 0)
		goto err;
	if ((ret = ws_source_open_namespace(
	    wtds, session, uri, "cache", ks->kvs_device, &ws->kvscache)) != 0)
		goto err;
	if ((ret = kvs_commit(ws->kvs)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_commit: %s", kvs_strerror(ret));
		goto err;
	}

	/* Optionally trade the global lock for the object. */
	if (!(flags & WS_SOURCE_OPEN_GLOBAL) &&
	    (ret = writelock(wtext, session, &ws->lock)) != 0)
		goto err;

	/* Insert the new entry at the head of the list. */
	ws->next = ks->ws_head;
	ks->ws_head = ws;

	*refp = ws;
	ws = NULL;

err:	if (ws != NULL)
		ETRET(ws_source_close(wtext, session, ws));

	/*
	 * If there was an error or our caller doesn't need the global lock,
	 * release the global lock.
	 */
	if (!(flags & WS_SOURCE_OPEN_GLOBAL) || ret != 0)
		ETRET(unlock(wtext, session, &ds->global_lock));
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

	if ((ret = wtext->config_get(		/* Parse configuration */
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

	if ((ret = wtext->collator_config(wtext, session, config)) != 0) {
		ESET(wtext, session, ret,
		    "collator configuration: %s", wtext->strerror(ret));
		goto err;
	}

	/* Finish initializing the cursor. */
	cursor->wtcursor.close = kvs_cursor_close;
	cursor->wtcursor.insert = kvs_cursor_insert;
	cursor->wtcursor.next = kvs_cursor_next;
	cursor->wtcursor.prev = kvs_cursor_prev;
	cursor->wtcursor.remove = kvs_cursor_remove;
	cursor->wtcursor.reset = kvs_cursor_reset;
	cursor->wtcursor.search = kvs_cursor_search;
	cursor->wtcursor.search_near = kvs_cursor_search_near;
	cursor->wtcursor.update = kvs_cursor_update;

	/*
	 * XXX
	 * The commit/rollback methods are private, which isn't right, but
	 * they don't appear in anything other than data-source cursors.
	 */
	cursor->wtcursor.commit = kvs_cursor_commit;
	cursor->wtcursor.rollback = kvs_cursor_rollback;

	cursor->wtext = wtext;

	cursor->record.key = cursor->key;
	if ((cursor->v = malloc(128)) == NULL)
		goto err;
	cursor->mem_len = 128;

	/* Get a locked reference to the WiredTiger source. */
	if ((ret = ws_source_open(wtds, session, uri, config, 0, &ws)) != 0)
		goto err;
	locked = 1;
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
		if (cursor != NULL)
			cursor_destroy(cursor);
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
	if ((ret = ws_source_open(wtds, session, uri, config, 0, &ws)) != 0)
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
	 * Get a locked reference to the data source: hold the global lock,
	 * we are going to change the list of objects for a KVS store.
	 *
	 * Remove the entry from the WT_SOURCE list -- it's a singly-linked
	 * list, find the reference to it.
	 */
	if ((ret = ws_source_open(wtds, session, uri, config,
	    WS_SOURCE_OPEN_BUSY | WS_SOURCE_OPEN_GLOBAL, &ws)) != 0)
		return (ret);
	ks = ws->ks;
	for (p = &ks->ws_head; *p != NULL; p = &(*p)->next)
		if (*p == ws) {
			*p = (*p)->next;
			break;
		}

	/* Close the source, discarding the handles and structure. */
	ETRET(ws_source_close(wtext, session, ws));
	ws = NULL;

	/* Drop the underlying namespaces. */
	ETRET(ws_source_drop_namespace(
	    wtds, session, uri, NULL, ks->kvs_device));
	ETRET(ws_source_drop_namespace(
	    wtds, session, uri, "cache", ks->kvs_device));

	/* Push the change. */
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

	ETRET(unlock(wtext, session, &ds->global_lock));
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

	/*
	 * Get a locked reference to the data source; hold the global lock,
	 * we are going to change the object's name, and we can't allow
	 * other threads walking the list and comparing against the name.
	 */
	if ((ret = ws_source_open(wtds, session, uri, config,
	    WS_SOURCE_OPEN_BUSY | WS_SOURCE_OPEN_GLOBAL, &ws)) != 0)
		return (ret);
	ks = ws->ks;

	/* Get a copy of the new name. */
	if ((copy = strdup(newuri)) == NULL) {
		ret = os_errno();
		goto err;
	}
	free(ws->uri);
	ws->uri = copy;
	copy = NULL;

	/* Rename the underlying namespaces. */
	ETRET(ws_source_rename_namespace(
	    wtds, session, uri, newuri, NULL, ks->kvs_device));
	ETRET(ws_source_rename_namespace(
	    wtds, session, uri, newuri, "cache", ks->kvs_device));

	/* Push the change. */
	if ((ret = kvs_commit(ws->kvs)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_commit: %s", kvs_strerror(ret));

	/* Update the metadata record. */
	ETRET(master_uri_rename(wtds, session, uri, newuri));

	/*
	 * If we have an error at this point, panic -- there's an inconsistency
	 * in what WiredTiger knows about and the underlying store.
	 */
	if (ret != 0)
		ret = WT_PANIC;

err:	ETRET(unlock(wtext, session, &ds->global_lock));

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
	if ((ret = ws_source_open(wtds, session,
	    uri, config, WS_SOURCE_OPEN_BUSY, &ws)) != 0)
		return (ret);

	/* Truncate the underlying namespaces. */
	if ((ret = kvs_truncate(ws->kvs)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_truncate: %s: %s", ws->uri, kvs_strerror(ret));
	if ((ret = kvs_truncate(ws->kvscache)) != 0)
		ESET(wtext, session, WT_ERROR,
		    "kvs_truncate: %s: %s", ws->uri, kvs_strerror(ret));

	ETRET(unlock(wtext, session, &ws->lock));
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
			ks->ws_head = ws->next;
			ETRET(ws_source_close(wtext, session, ws));
		}

		ds->kvs_head = ks->next;
		ETRET(kvs_source_close(wtext, session, ks));
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
