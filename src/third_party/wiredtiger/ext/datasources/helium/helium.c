/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
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
#include <sys/select.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <he.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

typedef struct he_env	HE_ENV;
typedef struct he_item	HE_ITEM;
typedef struct he_stats	HE_STATS;

static int verbose = 0;					/* Verbose messages */

/*
 * Macros to output error  and verbose messages, and set or return an error.
 * Error macros require local "ret" variable.
 *
 * ESET: update an error value, handling more/less important errors.
 * ERET: output a message, return the error.
 * EMSG: output a message, set the local error value.
 * EMSG_ERR:
 *	 output a message, set the local error value, jump to the err label.
 * VMSG: verbose message.
 */
#undef	ESET
#define	ESET(a) do {							\
	int __v;							\
	if ((__v = (a)) != 0) {						\
		/*							\
		 * On error, check for a panic (it overrides all other	\
		 * returns).  Else, if there's no return value or the	\
		 * return value is not strictly an error, override it	\
		 * with the error.					\
		 */							\
		if (__v == WT_PANIC ||					\
		    ret == 0 ||						\
		    ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND)	\
			ret = __v;					\
		/*							\
		 * If we're set to a Helium error at the end of the day,\
		 * switch to a generic WiredTiger error.		\
		 */							\
		if (ret < 0 && ret > -31,800)				\
			ret = WT_ERROR;					\
	}								\
} while (0)
#undef	ERET
#define	ERET(wtext, session, v, ...) do {				\
	(void)								\
	    wtext->err_printf(wtext, session, "helium: " __VA_ARGS__);	\
	ESET(v);							\
	return (ret);							\
} while (0)
#undef	EMSG
#define	EMSG(wtext, session, v, ...) do {				\
	(void)								\
	    wtext->err_printf(wtext, session, "helium: " __VA_ARGS__);	\
	ESET(v);							\
} while (0)
#undef	EMSG_ERR
#define	EMSG_ERR(wtext, session, v, ...) do {				\
	(void)								\
	    wtext->err_printf(wtext, session, "helium: " __VA_ARGS__);	\
	ESET(v);							\
	goto err;							\
} while (0)
#undef	VERBOSE_L1
#define	VERBOSE_L1	1
#undef	VERBOSE_L2
#define	VERBOSE_L2	2
#undef	VMSG
#define	VMSG(wtext, session, v, ...) do {				\
	if (verbose >= v)						\
		(void)wtext->						\
		    msg_printf(wtext, session, "helium: " __VA_ARGS__);	\
} while (0)

/*
 * OVERWRITE_AND_FREE --
 *	Make sure we don't re-use a structure after it's dead.
 */
#undef	OVERWRITE_AND_FREE
#define	OVERWRITE_AND_FREE(p) do {					\
	memset(p, 0xab, sizeof(*(p)));                         		\
	free(p);							\
} while (0)

/*
 * Version each object, out of sheer raging paranoia.
 */
#define	WIREDTIGER_HELIUM_MAJOR	1		/* Major, minor version */
#define	WIREDTIGER_HELIUM_MINOR	0

/*
 * WiredTiger name space on the Helium store: all objects are named with the
 * WiredTiger prefix (we don't require the Helium store be exclusive to our
 * files).  Primary objects are named "WiredTiger.[name]", associated cache
 * objects are "WiredTiger.[name].cache".  The per-connection transaction
 * object is "WiredTiger.WiredTigerTxn".  When we first open a Helium volume,
 * we open/close a file in order to apply flags for the first open of the
 * volume, that's "WiredTiger.WiredTigerInit".
 */
#define	WT_NAME_PREFIX	"WiredTiger."
#define	WT_NAME_INIT	"WiredTiger.WiredTigerInit"
#define	WT_NAME_TXN	"WiredTiger.WiredTigerTxn"
#define	WT_NAME_CACHE	".cache"

/*
 * WT_SOURCE --
 *	A WiredTiger source, supporting one or more cursors.
 */
typedef struct __wt_source {
	char *uri;				/* Unique name */

	pthread_rwlock_t lock;			/* Lock */
	int		 lockinit;		/* Lock created */

	int	configured;			/* If structure configured */
	u_int	ref;				/* Active reference count */

	uint64_t append_recno;			/* Allocation record number */

	int	 config_bitfield;		/* config "value_format=#t" */
	int	 config_compress;		/* config "helium_o_compress" */
	int	 config_recno;			/* config "key_format=r" */

	/*
	 * Each WiredTiger object has a "primary" namespace in a Helium store
	 * plus a "cache" namespace, which has not-yet-resolved updates.  There
	 * is a dirty flag so read-only data sets can ignore the cache.
	 */
	he_t	he;				/* Underlying Helium object */
	he_t	he_cache;			/* Underlying Helium cache */
	int	he_cache_inuse;

	struct __he_source *hs;			/* Underlying Helium source */
	struct __wt_source *next;		/* List of WiredTiger objects */
} WT_SOURCE;

/*
 * HELIUM_SOURCE --
 *	A Helium volume, supporting one or more WT_SOURCE objects.
 */
typedef struct __he_source {
	/*
	 * XXX
	 * The transaction commit handler must appear first in the structure.
	 */
	WT_TXN_NOTIFY txn_notify;		/* Transaction commit handler */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	char *name;				/* Unique WiredTiger name */
	char *device;				/* Unique Helium volume name */

	/*
	 * Maintain a handle for each underlying Helium source so checkpoint is
	 * faster, we can "commit" a single handle per source, regardless of the
	 * number of objects.
	 */
	he_t he_volume;

	struct __wt_source *ws_head;		/* List of WiredTiger sources */

	/*
	 * Each Helium source has a cleaner thread to migrate WiredTiger source
	 * updates from the cache namespace to the primary namespace, based on
	 * the number of bytes or the number of operations.  (There's a cleaner
	 * thread per Helium store so migration operations can overlap.)  We
	 * read these fields without a lock, but serialize writes to minimize
	 * races (and because it costs us nothing).
	 */
	pthread_t cleaner_id;			/* Cleaner thread ID */
	volatile int cleaner_stop;		/* Cleaner thread quit flag */

	/*
	 * Each WiredTiger connection has a transaction namespace which lists
	 * resolved transactions with their committed or aborted state as a
	 * value.  That namespace appears in a single Helium store (the first
	 * one created, if it doesn't already exist), and then it's referenced
	 * from other Helium stores.
	 */
#define	TXN_ABORTED	'A'
#define	TXN_COMMITTED	'C'
#define	TXN_UNRESOLVED	0
	he_t	he_txn;				/* Helium txn store */
	int	he_owner;			/* Owns transaction store */

	struct __he_source *next;		/* List of Helium sources */
} HELIUM_SOURCE;

/*
 * DATA_SOURCE --
 *	A WiredTiger data source, supporting one or more HELIUM_SOURCE objects.
 */
typedef struct __data_source {
	WT_DATA_SOURCE wtds;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	pthread_rwlock_t global_lock;		/* Global lock */
	int		 lockinit;		/* Lock created */

	struct __he_source *hs_head;		/* List of Helium sources */
} DATA_SOURCE;

/*
 * CACHE_RECORD --
 *	An array of updates from the cache object.
 *
 * Values in the cache store are marshalled/unmarshalled to/from the store,
 * using a simple encoding:
 *	{N records: 4B}
 *	{record#1 TxnID: 8B}
 *	{record#1 remove tombstone: 1B}
 *	{record#1 data length: 4B}
 *	{record#1 data}
 *	...
 *
 * Each cursor potentially has a single set of these values.
 */
typedef struct __cache_record {
	uint8_t	*v;				/* Value */
	uint32_t len;				/* Value length */
	uint64_t txnid;				/* Transaction ID */
#define	REMOVE_TOMBSTONE	'R'
	int	 remove;			/* 1/0 remove flag */
} CACHE_RECORD;

/*
 * CURSOR --
 *	A cursor, supporting a single WiredTiger cursor.
 */
typedef struct __cursor {
	WT_CURSOR wtcursor;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	WT_SOURCE *ws;				/* Underlying source */

	HE_ITEM record;				/* Record */
	uint8_t  __key[HE_MAX_KEY_LEN];		/* Record.key, Record.value */
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
 * prefix_match --
 *	Return if a string matches a prefix.
 */
static inline int
prefix_match(const char *str, const char *pfx)
{
	return (strncmp(str, pfx, strlen(pfx)) == 0);
}

/*
 * string_match --
 *	Return if a string matches a byte string of len bytes.
 */
static inline int
string_match(const char *str, const char *bytes, size_t len)
{
	return (strncmp(str, bytes, len) == 0 && (str)[(len)] == '\0');
}

/*
 * cursor_destroy --
 *	Free a cursor's memory, and optionally the cursor itself.
 */
static void
cursor_destroy(CURSOR *cursor)
{
	if (cursor != NULL) {
		free(cursor->v);
		free(cursor->t1.v);
		free(cursor->t2.v);
		free(cursor->t3.v);
		free(cursor->cache);
		OVERWRITE_AND_FREE(cursor);
	}
}

/*
 * os_errno --
 *	Limit our use of errno so it's easy to find/remove.
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
lock_init(WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_init(lockp, NULL)) != 0)
		ERET(wtext, session, WT_PANIC,
		    "pthread_rwlock_init: %s", strerror(ret));
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
		ERET(wtext, session, WT_PANIC,
		    "pthread_rwlock_destroy: %s", strerror(ret));
	return (0);
}

/*
 * writelock --
 *	Acquire a write lock.
 */
static inline int
writelock(WT_EXTENSION_API *wtext, WT_SESSION *session, pthread_rwlock_t *lockp)
{
	int ret = 0;

	if ((ret = pthread_rwlock_wrlock(lockp)) != 0)
		ERET(wtext, session, WT_PANIC,
		    "pthread_rwlock_wrlock: %s", strerror(ret));
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
		ERET(wtext, session, WT_PANIC,
		    "pthread_rwlock_unlock: %s", strerror(ret));
	return (0);
}

#if 0
/*
 * helium_dump_kv --
 *	Dump a Helium record.
 */
static void
helium_dump_kv(const char *pfx, uint8_t *p, size_t len, FILE *fp)
{
	(void)fprintf(stderr, "%s %3zu: ", pfx, len);
	for (; len > 0; --len, ++p)
		if (!isspace(*p) && isprint(*p))
			(void)putc(*p, fp);
		else if (len == 1 && *p == '\0')	/* Skip string nuls. */
			continue;
		else
			(void)fprintf(fp, "%#x", *p);
	(void)putc('\n', fp);
}

/*
 * helium_dump --
 *	Dump the records in a Helium store.
 */
static int
helium_dump(WT_EXTENSION_API *wtext, he_t he, const char *tag)
{
	HE_ITEM *r, _r;
	uint8_t k[4 * 1024], v[4 * 1024];
	int ret = 0;

	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = k;
	r->val = v;

	(void)fprintf(stderr, "== %s\n", tag);
	while ((ret = he_next(he, r, (size_t)0, sizeof(v))) == 0) {
#if 0
		uint64_t recno;
		if ((ret = wtext->struct_unpack(wtext,
		    NULL, r->key, r->key_len, "r", &recno)) != 0)
			return (ret);
		fprintf(stderr, "K: %" PRIu64, recno);
#else
		helium_dump_kv("K: ", r->key, r->key_len, stderr);
#endif
		helium_dump_kv("V: ", r->val, r->val_len, stderr);
	}
	if (ret != HE_ERR_ITEM_NOT_FOUND) {
		fprintf(stderr, "he_next: %s\n", he_strerror(ret));
		ret = WT_ERROR;
	}
	return (ret);
}

/*
 * helium_stats --
 *	Display Helium statistics for a datastore.
 */
static int
helium_stats(
    WT_EXTENSION_API *wtext, WT_SESSION *session, he_t he, const char *tag)
{
	HE_STATS stats;
	int ret = 0;

	if ((ret = he_stats(he, &stats)) != 0)
		ERET(wtext, session, ret, "he_stats: %s", he_strerror(ret));
	fprintf(stderr, "== %s\n", tag);
	fprintf(stderr, "name=%s\n", stats.name);
	fprintf(stderr, "deleted_items=%" PRIu64 "\n", stats.deleted_items);
	fprintf(stderr, "locked_items=%" PRIu64 "\n", stats.locked_items);
	fprintf(stderr, "valid_items=%" PRIu64 "\n", stats.valid_items);
	fprintf(stderr, "capacity=%" PRIu64 "B\n", stats.capacity);
	fprintf(stderr, "size=%" PRIu64 "B\n", stats.size);
	return (0);
}
#endif

/*
 * helium_call --
 *	Call a Helium key retrieval function, handling overflow.
 */
static inline int
helium_call(WT_CURSOR *wtcursor, const char *fname,
    he_t he, int (*f)(he_t, HE_ITEM *, size_t, size_t))
{
	CURSOR *cursor;
	HE_ITEM *r;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;
	char *p;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	r = &cursor->record;
	r->val = cursor->v;

restart:
	if ((ret = f(he, r, (size_t)0, cursor->mem_len)) != 0) {
		if (ret == HE_ERR_ITEM_NOT_FOUND)
			return (WT_NOTFOUND);
		ERET(wtext, session, ret, "%s: %s", fname, he_strerror(ret));
	}

	/*
	 * If the returned length is larger than our passed-in length, we didn't
	 * get the complete value.  Grow the buffer and use he_lookup to do the
	 * retrieval (he_lookup because the call succeeded and the key was
	 * copied out, so calling he_next/he_prev again would skip key/value
	 * pairs).
	 *
	 * We have to loop, another thread of control might change the length of
	 * the value, requiring we grow our buffer multiple times.
	 *
	 * We have to potentially restart the entire call in case the underlying
	 * key/value disappears.
	 */
	for (;;) {
		if (cursor->mem_len >= r->val_len) {
			cursor->len = r->val_len;
			return (0);
		}

		/* Grow the value buffer. */
		if ((p = realloc(cursor->v, r->val_len + 32)) == NULL)
			return (os_errno());
		cursor->v = r->val = p;
		cursor->mem_len = r->val_len + 32;

		if ((ret = he_lookup(he, r, (size_t)0, cursor->mem_len)) != 0) {
			if (ret == HE_ERR_ITEM_NOT_FOUND)
				goto restart;
			ERET(wtext,
			    session, ret, "he_lookup: %s", he_strerror(ret));
		}
	}
	/* NOTREACHED */
}

/*
 * txn_state_set --
 *	Resolve a transaction.
 */
static int
txn_state_set(WT_EXTENSION_API *wtext,
    WT_SESSION *session, HELIUM_SOURCE *hs, uint64_t txnid, int commit)
{
	HE_ITEM txn;
	uint8_t val;
	int ret = 0;

	/*
	 * Update the store -- commits must be durable, flush the volume.
	 *
	 * XXX
	 * Not endian-portable, we're writing a native transaction ID to the
	 * store.
	 */
	memset(&txn, 0, sizeof(txn));
	txn.key = &txnid;
	txn.key_len = sizeof(txnid);
	val = commit ? TXN_COMMITTED : TXN_ABORTED;
	txn.val = &val;
	txn.val_len = sizeof(val);

	if ((ret = he_update(hs->he_txn, &txn)) != 0)
		ERET(wtext, session, ret, "he_update: %s", he_strerror(ret));

	if (commit && (ret = he_commit(hs->he_txn)) != 0)
		ERET(wtext, session, ret, "he_commit: %s", he_strerror(ret));
	return (0);
}

/*
 * txn_notify --
 *	Resolve a transaction; called from WiredTiger during commit/abort.
 */
static int
txn_notify(WT_TXN_NOTIFY *handler,
    WT_SESSION *session, uint64_t txnid, int committed)
{
	HELIUM_SOURCE *hs;

	hs = (HELIUM_SOURCE *)handler;
	return (txn_state_set(hs->wtext, session, hs, txnid, committed));
}

/*
 * txn_state --
 *	Return a transaction's state.
 */
static int
txn_state(WT_CURSOR *wtcursor, uint64_t txnid)
{
	CURSOR *cursor;
	HE_ITEM txn;
	HELIUM_SOURCE *hs;
	uint8_t val_buf[16];

	cursor = (CURSOR *)wtcursor;
	hs = cursor->ws->hs;

	memset(&txn, 0, sizeof(txn));
	txn.key = &txnid;
	txn.key_len = sizeof(txnid);
	txn.val = val_buf;
	txn.val_len = sizeof(val_buf);

	if (he_lookup(hs->he_txn, &txn, (size_t)0, sizeof(val_buf)) == 0)
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
	CURSOR *cursor;
	HE_ITEM *r;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	uint64_t txnid;
	size_t len;
	uint32_t entries;
	uint8_t *p;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	r = &cursor->record;

	/*
	 * A cache update is 4B that counts the number of entries in the update,
	 * followed by sets of: 8B of txn ID then either a remove tombstone or a
	 * 4B length and variable-length data pair.  Grow the value buffer, then
	 * append the cursor's information.
	 */
	len = cursor->len +				/* current length */
	    sizeof(uint32_t) +				/* entries */
	    sizeof(uint64_t) +				/* txn ID */
	    1 +						/* remove byte */
	    (remove_op ? 0 :				/* optional data */
	    sizeof(uint32_t) + wtcursor->value.size) +
	    32;						/* slop */

	if (len > cursor->mem_len) {
		if ((p = realloc(cursor->v, len)) == NULL)
			return (os_errno());
		cursor->v = p;
		cursor->mem_len = len;
	}

	/* Get the transaction ID. */
	txnid = wtext->transaction_id(wtext, session);

	/* Update the number of records in this value. */
	if (cursor->len == 0) {
		entries = 1;
		cursor->len = sizeof(uint32_t);
	} else {
		memcpy(&entries, cursor->v, sizeof(uint32_t));
		++entries;
	}
	memcpy(cursor->v, &entries, sizeof(uint32_t));

	/*
	 * Copy the WiredTiger cursor's data into place: txn ID, remove
	 * tombstone, data length, data.
	 *
	 * XXX
	 * Not endian-portable, we're writing a native transaction ID to the
	 * store.
	 */
	p = cursor->v + cursor->len;
	memcpy(p, &txnid, sizeof(uint64_t));
	p += sizeof(uint64_t);
	if (remove_op)
		*p++ = REMOVE_TOMBSTONE;
	else {
		*p++ = ' ';
		memcpy(p, &wtcursor->value.size, sizeof(uint32_t));
		p += sizeof(uint32_t);
		memcpy(p, wtcursor->value.data, wtcursor->value.size);
		p += wtcursor->value.size;
	}
	cursor->len = (size_t)(p - cursor->v);

	/* Update the underlying Helium record. */
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
 * cache_value_aborted --
 *	Return if a transaction has been aborted.
 */
static inline int
cache_value_aborted(WT_CURSOR *wtcursor, CACHE_RECORD *cp)
{
	/*
	 * This function exists as a place to hang this comment.
	 *
	 * WiredTiger resets updated entry transaction IDs to an aborted state
	 * on rollback; to do that here would require tracking updated entries
	 * for a transaction or scanning the cache for updates made on behalf
	 * of the transaction during rollback, expensive stuff.  Instead, check
	 * if the transaction has been aborted before calling the underlying
	 * WiredTiger visibility function.
	 */
	return (txn_state(wtcursor, cp->txnid) == TXN_ABORTED ? 1 : 0);
}

/*
 * cache_value_committed --
 *	Return if a transaction has been committed.
 */
static inline int
cache_value_committed(WT_CURSOR *wtcursor, CACHE_RECORD *cp)
{
	return (txn_state(wtcursor, cp->txnid) == TXN_COMMITTED ? 1 : 0);
}

/*
 * cache_value_update_check --
 *	Return if an update can proceed based on the previous updates made to
 * the cache entry.
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
	if (wtext->
	    transaction_isolation_level(wtext, session) != WT_TXN_ISO_SNAPSHOT)
		return (0);

	/*
	 * If there's an entry that's not visible and hasn't been aborted,
	 * return a deadlock.
	 */
	for (i = 0, cp = cursor->cache; i < cursor->cache_entries; ++i, ++cp)
		if (!cache_value_aborted(wtcursor, cp) &&
		    !wtext->transaction_visible(wtext, session, cp->txnid))
			return (WT_ROLLBACK);
	return (0);
}

/*
 * cache_value_visible --
 *	Return the most recent cache entry update visible to the running
 * transaction.
 */
static int
cache_value_visible(WT_CURSOR *wtcursor, CACHE_RECORD **cpp)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	u_int i;

	*cpp = NULL;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;

	/*
	 * We want the most recent cache entry update; the cache entries are
	 * in update order, walk from the end to the beginning.
	 */
	cp = cursor->cache + cursor->cache_entries;
	for (i = 0; i < cursor->cache_entries; ++i) {
		--cp;
		if (!cache_value_aborted(wtcursor, cp) &&
		    wtext->transaction_visible(wtext, session, cp->txnid)) {
			*cpp = cp;
			return (1);
		}
	}
	return (0);
}

/*
 * cache_value_visible_all --
 *	Return if a cache entry has no updates that aren't globally visible.
 */
static int
cache_value_visible_all(WT_CURSOR *wtcursor, uint64_t oldest)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	u_int i;

	cursor = (CURSOR *)wtcursor;

	/*
	 * Compare the update's transaction ID and the oldest transaction ID
	 * not yet visible to a running transaction.  If there's an update a
	 * running transaction might want, the entry must remain in the cache.
	 * (We could tighten this requirement: if the only update required is
	 * also the update we'd migrate to the primary, it would still be OK
	 * to migrate it.)
	 */
	for (i = 0, cp = cursor->cache; i < cursor->cache_entries; ++i, ++cp)
		if (cp->txnid >= oldest)
			return (0);
	return (1);
}

/*
 * cache_value_last_committed --
 *	Find the most recent update in a cache entry, recovery processing.
 */
static void
cache_value_last_committed(WT_CURSOR *wtcursor, CACHE_RECORD **cpp)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	u_int i;

	*cpp = NULL;

	cursor = (CURSOR *)wtcursor;

	/*
	 * Find the most recent update in the cache record, we're going to try
	 * and migrate it into the primary, recovery version.
	 *
	 * We know the entry is visible, but it must have been committed before
	 * the failure to be migrated.
	 *
	 * Cache entries are in update order, walk from end to beginning.
	 */
	cp = cursor->cache + cursor->cache_entries;
	for (i = 0; i < cursor->cache_entries; ++i) {
		--cp;
		if (cache_value_committed(wtcursor, cp)) {
			*cpp = cp;
			return;
		}
	}
}

/*
 * cache_value_last_not_aborted --
 *	Find the most recent update in a cache entry, normal processing.
 */
static void
cache_value_last_not_aborted(WT_CURSOR *wtcursor, CACHE_RECORD **cpp)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	u_int i;

	*cpp = NULL;

	cursor = (CURSOR *)wtcursor;

	/*
	 * Find the most recent update in the cache record, we're going to try
	 * and migrate it into the primary, normal processing version.
	 *
	 * We don't have to check if the entry was committed, we've already
	 * confirmed all entries for this cache key are globally visible, which
	 * means they must be either committed or aborted.
	 *
	 * Cache entries are in update order, walk from end to beginning.
	 */
	cp = cursor->cache + cursor->cache_entries;
	for (i = 0; i < cursor->cache_entries; ++i) {
		--cp;
		if (!cache_value_aborted(wtcursor, cp)) {
			*cpp = cp;
			return;
		}
	}
}

/*
 * cache_value_txnmin --
 *	Return the oldest transaction ID involved in a cache update.
 */
static void
cache_value_txnmin(WT_CURSOR *wtcursor, uint64_t *txnminp)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	uint64_t txnmin;
	u_int i;

	cursor = (CURSOR *)wtcursor;

	/* Return the oldest transaction ID for in the cache entry. */
	txnmin = UINT64_MAX;
	for (i = 0, cp = cursor->cache; i < cursor->cache_entries; ++i, ++cp)
		if (txnmin > cp->txnid)
			txnmin = cp->txnid;
	*txnminp = txnmin;
}

/*
 * key_max_err --
 *	Common error when a WiredTiger key is too large.
 */
static int
key_max_err(WT_EXTENSION_API *wtext, WT_SESSION *session, size_t len)
{
	int ret = 0;

	ERET(wtext, session, EINVAL,
	    "key length (%zu bytes) larger than the maximum Helium "
	    "key length of %d bytes",
	    len, HE_MAX_KEY_LEN);
}

/*
 * copyin_key --
 *	Copy a WT_CURSOR key to a HE_ITEM key.
 */
static inline int
copyin_key(WT_CURSOR *wtcursor, int allocate_key)
{
	CURSOR *cursor;
	HE_ITEM *r;
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
		    (ret = wtext->struct_pack(wtext, session,
		    r->key, HE_MAX_KEY_LEN, "r", wtcursor->recno)) != 0)
			return (ret);
		r->key_len = size;
	} else {
		/* I'm not sure this test is necessary, but it's cheap. */
		if (wtcursor->key.size > HE_MAX_KEY_LEN)
			return (
			    key_max_err(wtext, session, wtcursor->key.size));

		/*
		 * A set cursor key might reference application memory, which
		 * is only OK until the cursor operation has been called (in
		 * other words, we can only reference application memory from
		 * the WT_CURSOR.set_key call until the WT_CURSOR.op call).
		 * For this reason, do a full copy, don't just reference the
		 * WT_CURSOR key's data.
		 */
		memcpy(r->key, wtcursor->key.data, wtcursor->key.size);
		r->key_len = wtcursor->key.size;
	}
	return (0);
}

/*
 * copyout_key --
 *	Copy a HE_ITEM key to a WT_CURSOR key.
 */
static inline int
copyout_key(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	HE_ITEM *r;
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
		wtcursor->key.size = (size_t)r->key_len;
	}
	return (0);
}

/*
 * copyout_val --
 *	Copy a Helium store's HE_ITEM value to a WT_CURSOR value.
 */
static inline int
copyout_val(WT_CURSOR *wtcursor, CACHE_RECORD *cp)
{
	CURSOR *cursor;

	cursor = (CURSOR *)wtcursor;

	if (cp == NULL) {
		wtcursor->value.data = cursor->v;
		wtcursor->value.size = cursor->len;
	} else {
		wtcursor->value.data = cp->v;
		wtcursor->value.size = cp->len;
	}
	return (0);
}

/*
 * nextprev --
 *	Cursor next/prev.
 */
static int
nextprev(WT_CURSOR *wtcursor, const char *fname,
    int (*f)(he_t, HE_ITEM *, size_t, size_t))
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	HE_ITEM *r;
	WT_EXTENSION_API *wtext;
	WT_ITEM a, b;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int cache_ret, cache_rm, cmp, ret = 0;
	void *p;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	ws = cursor->ws;
	wtext = cursor->wtext;
	r = &cursor->record;

	cache_rm = 0;

	/*
	 * If the cache isn't yet in use, it's a simpler problem, just check
	 * the store.  We don't care if we race, we're not guaranteeing any
	 * special behavior with respect to phantoms.
	 */
	if (ws->he_cache_inuse == 0) {
		cache_ret = WT_NOTFOUND;
		goto cache_clean;
	}

skip_deleted:
	/*
	 * The next/prev key/value pair might be in the cache, which means we
	 * are making two calls and returning the best choice.  As each call
	 * overwrites both key and value, we have to have a copy of the key
	 * for the second call plus the returned key and value from the first
	 * call. That's why each cursor has 3 temporary buffers.
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
	for (cache_rm = 0;;) {
		if ((ret = helium_call(wtcursor, fname, ws->he_cache, f)) != 0)
			break;
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			return (ret);

		/* If there's no visible entry, move to the next one. */
		if (!cache_value_visible(wtcursor, &cp))
			continue;

		/*
		 * If the entry has been deleted, remember that and continue.
		 * We can't just skip the entry because it might be a delete
		 * of an entry in the primary store, which means the cache
		 * entry stops us from returning the primary store's entry.
		 */
		if (cp->remove)
			cache_rm = 1;

		/*
		 * Copy the cache key. If the cache's entry wasn't a delete,
		 * copy the value as well, we may return the cache entry.
		 */
		if (cursor->t2.mem_len < r->key_len) {
			if ((p = realloc(cursor->t2.v, r->key_len)) == NULL)
				return (os_errno());
			cursor->t2.v = p;
			cursor->t2.mem_len = r->key_len;
		}
		memcpy(cursor->t2.v, r->key, r->key_len);
		cursor->t2.len = r->key_len;

		if (cache_rm)
			break;

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
	ret = helium_call(wtcursor, fname, ws->he, f);
	if (ret != 0 && ret != WT_NOTFOUND)
		return (ret);

	/* If no entries in either the cache or the primary, we're done. */
	if (cache_ret == WT_NOTFOUND && ret == WT_NOTFOUND)
		return (WT_NOTFOUND);

	/*
	 * If both the cache and the primary had entries, decide which is a
	 * better choice and pretend we didn't find the other one.
	 */
	if (cache_ret == 0 && ret == 0) {
		a.data = r->key;		/* a is the primary */
		a.size = (uint32_t)r->key_len;
		b.data = cursor->t2.v;		/* b is the cache */
		b.size = (uint32_t)cursor->t2.len;
		if ((ret = wtext->collate(
		    wtext, session, NULL, &a, &b, &cmp)) != 0)
			return (ret);

		if (f == he_next) {
			if (cmp >= 0)
				ret = WT_NOTFOUND;
			else
				cache_ret = WT_NOTFOUND;
		} else {
			if (cmp <= 0)
				ret = WT_NOTFOUND;
			else
				cache_ret = WT_NOTFOUND;
		}
	}

	/*
	 * If the cache is the key we'd choose, but it's a delete, skip past it
	 * by moving from the deleted key to the next/prev item in either the
	 * primary or the cache.
	 */
	if (cache_ret == 0 && cache_rm) {
		memcpy(r->key, cursor->t2.v, cursor->t2.len);
		r->key_len = cursor->t2.len;
		goto skip_deleted;
	}

	/* If taking the cache's entry, copy the value into place. */
	if (cache_ret == 0) {
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
 * helium_cursor_next --
 *	WT_CURSOR.next method.
 */
static int
helium_cursor_next(WT_CURSOR *wtcursor)
{
	return (nextprev(wtcursor, "he_next", he_next));
}

/*
 * helium_cursor_prev --
 *	WT_CURSOR.prev method.
 */
static int
helium_cursor_prev(WT_CURSOR *wtcursor)
{
	return (nextprev(wtcursor, "he_prev", he_prev));
}

/*
 * helium_cursor_reset --
 *	WT_CURSOR.reset method.
 */
static int
helium_cursor_reset(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	HE_ITEM *r;

	cursor = (CURSOR *)wtcursor;
	r = &cursor->record;

	/*
	 * Reset the cursor by setting the key length to 0, causing subsequent
	 * next/prev operations to return the first/last record of the object.
	 */
	r->key_len = 0;
	return (0);
}

/*
 * helium_cursor_search --
 *	WT_CURSOR.search method.
 */
static int
helium_cursor_search(WT_CURSOR *wtcursor)
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
	if ((ret =
	    helium_call(wtcursor, "he_lookup", ws->he_cache, he_lookup)) == 0) {
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			return (ret);
		if (cache_value_visible(wtcursor, &cp))
			return (cp->remove ?
			    WT_NOTFOUND : copyout_val(wtcursor, cp));
	} else if (ret != WT_NOTFOUND)
		return (ret);

	/* Check for an entry in the primary store. */
	if ((ret = helium_call(wtcursor, "he_lookup", ws->he, he_lookup)) != 0)
		return (ret);

	return (copyout_val(wtcursor, NULL));
}

/*
 * helium_cursor_search_near --
 *	WT_CURSOR.search_near method.
 */
static int
helium_cursor_search_near(WT_CURSOR *wtcursor, int *exact)
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
	if ((ret = helium_cursor_search(wtcursor)) == 0) {
		*exact = 0;
		return (0);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	/* Search for a key that's larger. */
	if ((ret = helium_cursor_next(wtcursor)) == 0) {
		*exact = 1;
		return (0);
	}
	if (ret != WT_NOTFOUND)
		return (ret);

	/* Search for a key that's smaller. */
	if ((ret = helium_cursor_prev(wtcursor)) == 0) {
		*exact = -1;
		return (0);
	}

	return (ret);
}

/*
 * helium_cursor_insert --
 *	WT_CURSOR.insert method.
 */
static int
helium_cursor_insert(WT_CURSOR *wtcursor)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	HE_ITEM *r;
	HELIUM_SOURCE *hs;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;
	hs = ws->hs;
	r = &cursor->record;

	/* Get the WiredTiger cursor's key. */
	if ((ret = copyin_key(wtcursor, 1)) != 0)
		return (ret);

	VMSG(wtext, session, VERBOSE_L2,
	    "I %.*s.%.*s", (int)r->key_len, r->key, (int)r->val_len, r->val);

	/* Clear the value, assume we're adding the first cache entry. */
	cursor->len = 0;

	/* Updates are read-modify-writes, lock the underlying cache. */
	if ((ret = writelock(wtext, session, &ws->lock)) != 0)
		return (ret);

	/* Read the record from the cache store. */
	switch (ret = helium_call(
	    wtcursor, "he_lookup", ws->he_cache, he_lookup)) {
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
		if ((ret = helium_call(
		    wtcursor, "he_lookup", ws->he, he_lookup)) != WT_NOTFOUND) {
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
	 * Create a new value using the current cache record plus the WiredTiger
	 * cursor's value, and update the cache.
	 */
	if ((ret = cache_value_append(wtcursor, 0)) != 0)
		goto err;
	if ((ret = he_update(ws->he_cache, r)) != 0)
		EMSG(wtext, session, ret, "he_update: %s", he_strerror(ret));

	/* Update the state while still holding the lock. */
	if (ws->he_cache_inuse == 0)
		ws->he_cache_inuse = 1;

	/* Discard the lock. */
err:	ESET(unlock(wtext, session, &ws->lock));

	/* If successful, request notification at transaction resolution. */
	if (ret == 0)
		ESET(
		    wtext->transaction_notify(wtext, session, &hs->txn_notify));

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
	HE_ITEM *r;
	HELIUM_SOURCE *hs;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	WT_SOURCE *ws;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ws = cursor->ws;
	hs = ws->hs;
	r = &cursor->record;

	/* Get the WiredTiger cursor's key. */
	if ((ret = copyin_key(wtcursor, 0)) != 0)
		return (ret);

	VMSG(wtext, session, VERBOSE_L2,
	    "%c %.*s.%.*s",
	    remove_op ? 'R' : 'U',
	    (int)r->key_len, r->key, (int)r->val_len, r->val);

	/* Clear the value, assume we're adding the first cache entry. */
	cursor->len = 0;

	/* Updates are read-modify-writes, lock the underlying cache. */
	if ((ret = writelock(wtext, session, &ws->lock)) != 0)
		return (ret);

	/* Read the record from the cache store. */
	switch (ret = helium_call(
	    wtcursor, "he_lookup", ws->he_cache, he_lookup)) {
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
		 * error. We're done checking if there is a visible entry in
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
		if ((ret =
		    helium_call(wtcursor, "he_lookup", ws->he, he_lookup)) != 0)
			goto err;

		/*
		 * All we care about is the cache entry, which didn't exist;
		 * clear the returned value, we're about to "append" to it.
		 */
		cursor->len = 0;
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
	if ((ret = he_update(ws->he_cache, r)) != 0)
		EMSG(wtext, session, ret, "he_update: %s", he_strerror(ret));

	/* Update the state while still holding the lock. */
	if (ws->he_cache_inuse == 0)
		ws->he_cache_inuse = 1;

	/* Discard the lock. */
err:	ESET(unlock(wtext, session, &ws->lock));

	/* If successful, request notification at transaction resolution. */
	if (ret == 0)
		ESET(
		    wtext->transaction_notify(wtext, session, &hs->txn_notify));

	return (ret);
}

/*
 * helium_cursor_update --
 *	WT_CURSOR.update method.
 */
static int
helium_cursor_update(WT_CURSOR *wtcursor)
{
	return (update(wtcursor, 0));
}

/*
 * helium_cursor_remove --
 *	WT_CURSOR.remove method.
 */
static int
helium_cursor_remove(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	WT_SOURCE *ws;

	cursor = (CURSOR *)wtcursor;
	ws = cursor->ws;

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of zero.
	 */
	if (ws->config_bitfield) {
		wtcursor->value.size = 1;
		wtcursor->value.data = "";
		return (update(wtcursor, 0));
	}
	return (update(wtcursor, 1));
}

/*
 * helium_cursor_close --
 *	WT_CURSOR.close method.
 */
static int
helium_cursor_close(WT_CURSOR *wtcursor)
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
 * ws_source_name --
 *	Build a namespace name.
 */
static int
ws_source_name(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, const char *suffix, char **pp)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	size_t len;
	int ret = 0;
	const char *p;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/*
	 * Create the store's name.  Application URIs are "helium:device/name";
	 * we want the names on the Helium device to be obviously WiredTiger's,
	 * and the device name isn't interesting.  Convert to "WiredTiger:name",
	 * and add an optional suffix.
	 */
	if (!prefix_match(uri, "helium:") || (p = strchr(uri, '/')) == NULL)
		ERET(wtext, session, EINVAL, "%s: illegal Helium URI", uri);
	++p;

	len = strlen(WT_NAME_PREFIX) +
	    strlen(p) + (suffix == NULL ? 0 : strlen(suffix)) + 5;
	if ((*pp = malloc(len)) == NULL)
		return (os_errno());
	(void)snprintf(*pp, len, "%s%s%s",
	    WT_NAME_PREFIX, p, suffix == NULL ? "" : suffix);
	return (0);
}

/*
 * ws_source_close --
 *	Close a WT_SOURCE reference.
 */
static int
ws_source_close(WT_EXTENSION_API *wtext, WT_SESSION *session, WT_SOURCE *ws)
{
	int ret = 0, tret;

	/*
	 * Warn if open cursors: it shouldn't happen because the upper layers of
	 * WiredTiger prevent it, so we don't do anything more than warn.
	 */
	if (ws->ref != 0)
		EMSG(wtext, session, WT_ERROR,
		    "%s: open object with %u open cursors being closed",
		    ws->uri, ws->ref);

	if (ws->he != NULL) {
		if ((tret = he_commit(ws->he)) != 0)
			EMSG(wtext, session, tret,
			    "he_commit: %s: %s", ws->uri, he_strerror(tret));
		if ((tret = he_close(ws->he)) != 0)
			EMSG(wtext, session, tret,
			    "he_close: %s: %s", ws->uri, he_strerror(tret));
		ws->he = NULL;
	}
	if (ws->he_cache != NULL) {
		if ((tret = he_close(ws->he_cache)) != 0)
			EMSG(wtext, session, tret,
			    "he_close: %s(cache): %s",
			    ws->uri, he_strerror(tret));
		ws->he_cache = NULL;
	}

	if (ws->lockinit)
		ESET(lock_destroy(wtext, session, &ws->lock));

	free(ws->uri);
	OVERWRITE_AND_FREE(ws);

	return (ret);
}

/*
 * ws_source_open_object --
 *	Open an object in the Helium store.
 */
static int
ws_source_open_object(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    HELIUM_SOURCE *hs,
    const char *uri, const char *suffix, int flags, he_t *hep)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	he_t he;
	char *p;
	int ret = 0;

	*hep = NULL;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	p = NULL;

	/* Open the underlying Helium object. */
	if ((ret = ws_source_name(wtds, session, uri, suffix, &p)) != 0)
		return (ret);
	VMSG(wtext, session, VERBOSE_L1, "open %s/%s", hs->name, p);
	if ((he = he_open(hs->device, p, flags, NULL)) == NULL) {
		ret = os_errno();
		EMSG(wtext, session, ret,
		    "he_open: %s/%s: %s", hs->name, p, he_strerror(ret));
	}
	*hep = he;

	free(p);
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
	HELIUM_SOURCE *hs;
	WT_CONFIG_ITEM a;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	size_t len;
	int oflags, ret = 0;
	const char *p, *t;

	*refp = NULL;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	ws = NULL;

	/*
	 * The URI will be "helium:" followed by a Helium name and object name
	 * pair separated by a slash, for example, "helium:volume/object".
	 */
	if (!prefix_match(uri, "helium:"))
		goto bad_name;
	p = uri + strlen("helium:");
	if (p[0] == '/' || (t = strchr(p, '/')) == NULL || t[1] == '\0')
bad_name:	ERET(wtext, session, EINVAL, "%s: illegal name format", uri);
	len = (size_t)(t - p);

	/* Find a matching Helium device. */
	for (hs = ds->hs_head; hs != NULL; hs = hs->next)
		if (string_match(hs->name, p, len))
			break;
	if (hs == NULL)
		ERET(wtext, NULL,
		    EINVAL, "%s: no matching Helium store found", uri);

	/*
	 * We're about to walk the Helium device's list of files, acquire the
	 * global lock.
	 */
	if ((ret = writelock(wtext, session, &ds->global_lock)) != 0)
		return (ret);

	/*
	 * Check for a match: if we find one, optionally trade the global lock
	 * for the object's lock, optionally check if the object is busy, and
	 * return.
	 */
	for (ws = hs->ws_head; ws != NULL; ws = ws->next)
		if (strcmp(ws->uri, uri) == 0) {
			/* Check to see if the object is busy. */
			if (ws->ref != 0 && (flags & WS_SOURCE_OPEN_BUSY)) {
				ret = EBUSY;
				ESET(unlock(wtext, session, &ds->global_lock));
				return (ret);
			}
			/* Swap the global lock for an object lock. */
			if (!(flags & WS_SOURCE_OPEN_GLOBAL)) {
				ret = writelock(wtext, session, &ws->lock);
				ESET(unlock(wtext, session, &ds->global_lock));
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
	ws->hs = hs;

	/*
	 * Open the underlying Helium objects, then push the change.
	 *
	 * The naming scheme is simple: the URI names the primary store, and the
	 * URI with a trailing suffix names the associated caching store.
	 *
	 * We can set truncate flag, we always set the create flag, our caller
	 * handles attempts to create existing objects.
	 */
	oflags = HE_O_CREATE;
	if ((ret = wtext->config_get(wtext,
	    session, config, "helium_o_truncate", &a)) == 0 && a.val != 0)
		oflags |= HE_O_TRUNCATE;
	if (ret != 0 && ret != WT_NOTFOUND)
		EMSG_ERR(wtext, session, ret,
		    "helium_o_truncate configuration: %s",
		    wtext->strerror(wtext, session, ret));

	if ((ret = ws_source_open_object(
	    wtds, session, hs, uri, NULL, oflags, &ws->he)) != 0)
		goto err;
	if ((ret = ws_source_open_object(
	    wtds, session, hs, uri, WT_NAME_CACHE, oflags, &ws->he_cache)) != 0)
		goto err;
	if ((ret = he_commit(ws->he)) != 0)
		EMSG_ERR(wtext, session, ret,
		    "he_commit: %s", he_strerror(ret));

	/* Optionally trade the global lock for the object lock. */
	if (!(flags & WS_SOURCE_OPEN_GLOBAL) &&
	    (ret = writelock(wtext, session, &ws->lock)) != 0)
		goto err;

	/* Insert the new entry at the head of the list. */
	ws->next = hs->ws_head;
	hs->ws_head = ws;

	*refp = ws;
	ws = NULL;

	if (0) {
err:		if (ws != NULL)
			ESET(ws_source_close(wtext, session, ws));
	}

	/*      
	 * If there was an error or our caller doesn't need the global lock,
	 * release the global lock.
	 */
	if (!(flags & WS_SOURCE_OPEN_GLOBAL) || ret != 0)
		ESET(unlock(wtext, session, &ds->global_lock));

	return (ret);
}

/*
 * master_uri_get --
 *	Get the Helium master record for a URI.
 */
static int
master_uri_get(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, char **valuep)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	return (wtext->metadata_search(wtext, session, uri, valuep));
}

/*
 * master_uri_drop --
 *	Drop the Helium master record for a URI.
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
 *	Rename the Helium master record for a URI.
 */
static int
master_uri_rename(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, const char *newuri)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	char *value;

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
 *	Set the Helium master record for a URI.
 */
static int
master_uri_set(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	WT_CONFIG_ITEM a, b, c;
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
		    "exclusive configuration: %s",
		    wtext->strerror(wtext, session, ret));

	/* Get the key/value format strings. */
	if ((ret = wtext->config_get(
	    wtext, session, config, "key_format", &a)) != 0) {
		if (ret == WT_NOTFOUND) {
			a.str = "u";
			a.len = 1;
		} else
			ERET(wtext, session, ret,
			    "key_format configuration: %s",
			    wtext->strerror(wtext, session, ret));
	}
	if ((ret = wtext->config_get(
	    wtext, session, config, "value_format", &b)) != 0) {
		if (ret == WT_NOTFOUND) {
			b.str = "u";
			b.len = 1;
		} else
			ERET(wtext, session, ret,
			    "value_format configuration: %s",
			    wtext->strerror(wtext, session, ret));
	}

	/* Get the compression configuration. */
	if ((ret = wtext->config_get(
	    wtext, session, config, "helium_o_compress", &c)) != 0) {
		if (ret == WT_NOTFOUND)
			c.val = 0;
		else
			ERET(wtext, session, ret,
			    "helium_o_compress configuration: %s",
			    wtext->strerror(wtext, session, ret));
	}

	/*
	 * Create a new reference using insert (which fails if the record
	 * already exists).
	 */
	(void)snprintf(value, sizeof(value),
	    "wiredtiger_helium_version=(major=%d,minor=%d),"
	    "key_format=%.*s,value_format=%.*s,"
	    "helium_o_compress=%d",
	    WIREDTIGER_HELIUM_MAJOR, WIREDTIGER_HELIUM_MINOR,
	    (int)a.len, a.str, (int)b.len, b.str, c.val ? 1 : 0);
	if ((ret = wtext->metadata_insert(wtext, session, uri, value)) == 0)
		return (0);
	if (ret == WT_DUPLICATE_KEY)
		return (exclusive ? EEXIST : 0);
	ERET(wtext,
	    session, ret, "%s: %s", uri, wtext->strerror(wtext, session, ret));
}

/*
 * helium_session_open_cursor --
 *	WT_SESSION.open_cursor method.
 */
static int
helium_session_open_cursor(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, WT_CURSOR **new_cursor)
{
	CURSOR *cursor;
	DATA_SOURCE *ds;
	WT_CONFIG_ITEM v;
	WT_CONFIG_PARSER *config_parser;
	WT_CURSOR *wtcursor;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int locked, own, ret, tret;
	char *value;

	*new_cursor = NULL;

	config_parser = NULL;
	cursor = NULL;
	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	ws = NULL;
	locked = 0;
	ret = tret = 0;
	value = NULL;

	/* Allocate and initialize a cursor. */
	if ((cursor = calloc(1, sizeof(CURSOR))) == NULL)
		return (os_errno());

	if ((ret = wtext->config_get(		/* Parse configuration */
	    wtext, session, config, "append", &v)) != 0)
		EMSG_ERR(wtext, session, ret,
		    "append configuration: %s",
		    wtext->strerror(wtext, session, ret));
	cursor->config_append = v.val != 0;

	if ((ret = wtext->config_get(
	    wtext, session, config, "overwrite", &v)) != 0)
		EMSG_ERR(wtext, session, ret,
		    "overwrite configuration: %s",
		    wtext->strerror(wtext, session, ret));
	cursor->config_overwrite = v.val != 0;

	if ((ret = wtext->collator_config(
	    wtext, session, uri, config, NULL, &own)) != 0)
		EMSG_ERR(wtext, session, ret,
		    "collator configuration: %s",
		    wtext->strerror(wtext, session, ret));

	/* Finish initializing the cursor. */
	cursor->wtcursor.close = helium_cursor_close;
	cursor->wtcursor.insert = helium_cursor_insert;
	cursor->wtcursor.next = helium_cursor_next;
	cursor->wtcursor.prev = helium_cursor_prev;
	cursor->wtcursor.remove = helium_cursor_remove;
	cursor->wtcursor.reset = helium_cursor_reset;
	cursor->wtcursor.search = helium_cursor_search;
	cursor->wtcursor.search_near = helium_cursor_search_near;
	cursor->wtcursor.update = helium_cursor_update;

	cursor->wtext = wtext;
	cursor->record.key = cursor->__key;
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

		if ((ret = wtext->config_parser_open(wtext,
		    session, value, strlen(value), &config_parser)) != 0)
			EMSG_ERR(wtext, session, ret,
			    "Configuration string parser: %s",
			    wtext->strerror(wtext, session, ret));
		if ((ret = config_parser->get(
		    config_parser, "key_format", &v)) != 0)
			EMSG_ERR(wtext, session, ret,
			    "key_format configuration: %s",
			    wtext->strerror(wtext, session, ret));
		ws->config_recno = v.len == 1 && v.str[0] == 'r';

		if ((ret = config_parser->get(
		    config_parser, "value_format", &v)) != 0)
			EMSG_ERR(wtext, session, ret,
			    "value_format configuration: %s",
			    wtext->strerror(wtext, session, ret));
		ws->config_bitfield = v.len == 2 &&
		    isdigit((u_char)v.str[0]) && v.str[1] == 't';

		if ((ret = config_parser->get(
		    config_parser, "helium_o_compress", &v)) != 0)
			EMSG_ERR(wtext, session, ret,
			    "helium_o_compress configuration: %s",
			    wtext->strerror(wtext, session, ret));
		ws->config_compress = v.val ? 1 : 0;

		/*
		 * If it's a record-number key, read the last record from the
		 * object and set the allocation record value.
		 */
		if (ws->config_recno) {
			wtcursor = (WT_CURSOR *)cursor;
			if ((ret = helium_cursor_reset(wtcursor)) != 0)
				goto err;

			if ((ret = helium_cursor_prev(wtcursor)) == 0)
				ws->append_recno = wtcursor->recno;
			else if (ret != WT_NOTFOUND)
				goto err;

			if ((ret = helium_cursor_reset(wtcursor)) != 0)
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
			ESET(unlock(wtext, session, &ws->lock));
		cursor_destroy(cursor);
	}
	if (config_parser != NULL &&
	    (tret = config_parser->close(config_parser)) != 0)
		EMSG(wtext, session, tret,
		    "WT_CONFIG_PARSER.close: %s",
		    wtext->strerror(wtext, session, tret));

	free((void *)value);
	return (ret);
}

/*
 * helium_session_create --
 *	WT_SESSION.create method.
 */
static int
helium_session_create(WT_DATA_SOURCE *wtds,
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
	 * We've discarded the lock, but that's OK, creates are single-threaded
	 * at the WiredTiger level, it's not our problem to solve.
	 *
	 * If unable to enter a WiredTiger record, leave the Helium store alone.
	 * A subsequent create should do the right thing, we aren't leaving
	 * anything in an inconsistent state.
	 */
	return (master_uri_set(wtds, session, uri, config));
}

/*
 * helium_session_drop --
 *	WT_SESSION.drop method.
 */
static int
helium_session_drop(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	HELIUM_SOURCE *hs;
	WT_EXTENSION_API *wtext;
	WT_SOURCE **p, *ws;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/*
	 * Get a locked reference to the data source: hold the global lock,
	 * we're changing the HELIUM_SOURCE's list of WT_SOURCE objects.
	 *
	 * Remove the entry from the WT_SOURCE list -- it's a singly-linked
	 * list, find the reference to it.
	 */
	if ((ret = ws_source_open(wtds, session, uri, config,
	    WS_SOURCE_OPEN_BUSY | WS_SOURCE_OPEN_GLOBAL, &ws)) != 0)
		return (ret);
	hs = ws->hs;
	for (p = &hs->ws_head; *p != NULL; p = &(*p)->next)
		if (*p == ws) {
			*p = (*p)->next;
			break;
		}

	/* Drop the underlying Helium objects. */
	ESET(he_remove(ws->he));
	ws->he = NULL;				/* The handle is dead. */
	ESET(he_remove(ws->he_cache));
	ws->he_cache = NULL;			/* The handle is dead. */

	/* Close the source, discarding the structure. */
	ESET(ws_source_close(wtext, session, ws));
	ws = NULL;

	/* Discard the metadata entry. */
	ESET(master_uri_drop(wtds, session, uri));

	/*
	 * If we have an error at this point, panic -- there's an inconsistency
	 * in what WiredTiger knows about and the underlying store.
	 */
	if (ret != 0)
		ret = WT_PANIC;

	ESET(unlock(wtext, session, &ds->global_lock));
	return (ret);
}

/*
 * helium_session_rename --
 *	WT_SESSION.rename method.
 */
static int
helium_session_rename(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    const char *uri, const char *newuri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int ret = 0;
	char *p;

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

	/* Get a copy of the new name for the WT_SOURCE structure. */
	if ((p = strdup(newuri)) == NULL) {
		ret = os_errno();
		goto err;
	}
	free(ws->uri);
	ws->uri = p;

	/* Rename the underlying Helium objects. */
	ESET(ws_source_name(wtds, session, newuri, NULL, &p));
	if (ret == 0) {
		ESET(he_rename(ws->he, p));
		free(p);
	}
	ESET(ws_source_name(wtds, session, newuri, WT_NAME_CACHE, &p));
	if (ret == 0) {
		ESET(he_rename(ws->he_cache, p));
		free(p);
	}

	/* Update the metadata record. */
	ESET(master_uri_rename(wtds, session, uri, newuri));

	/*
	 * If we have an error at this point, panic -- there's an inconsistency
	 * in what WiredTiger knows about and the underlying store.
	 */
	if (ret != 0)
		ret = WT_PANIC;

err:	ESET(unlock(wtext, session, &ds->global_lock));

	return (ret);
}

/*
 * helium_session_truncate --
 *	WT_SESSION.truncate method.
 */
static int
helium_session_truncate(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	int ret = 0, tret;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Get a locked reference to the WiredTiger source. */
	if ((ret = ws_source_open(wtds, session,
	    uri, config, WS_SOURCE_OPEN_BUSY, &ws)) != 0)
		return (ret);

	/* Truncate the underlying namespaces. */
	if ((tret = he_truncate(ws->he)) != 0)
		EMSG(wtext, session, tret,
		    "he_truncate: %s: %s", ws->uri, he_strerror(tret));
	if ((tret = he_truncate(ws->he_cache)) != 0)
		EMSG(wtext, session, tret,
		    "he_truncate: %s: %s", ws->uri, he_strerror(tret));

	ESET(unlock(wtext, session, &ws->lock));
	return (ret);
}

/*
 * helium_session_verify --
 *	WT_SESSION.verify method.
 */
static int
helium_session_verify(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
{
	(void)wtds;
	(void)session;
	(void)uri;
	(void)config;
	return (0);
}

/*
 * helium_session_checkpoint --
 *	WT_SESSION.checkpoint method.
 */
static int
helium_session_checkpoint(
    WT_DATA_SOURCE *wtds, WT_SESSION *session, WT_CONFIG_ARG *config)
{
	DATA_SOURCE *ds;
	HELIUM_SOURCE *hs;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	(void)config;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Flush all volumes. */
	if ((hs = ds->hs_head) != NULL &&
	    (ret = he_commit(hs->he_volume)) != 0)
		ERET(wtext, session, ret,
		    "he_commit: %s: %s", hs->device, he_strerror(ret));

	return (0);
}

/*
 * helium_source_close --
 *	Discard a HELIUM_SOURCE.
 */
static int
helium_source_close(
    WT_EXTENSION_API *wtext, WT_SESSION *session, HELIUM_SOURCE *hs)
{
	WT_SOURCE *ws;
	int ret = 0, tret;

	/* Resolve the cache into the primary one last time and quit. */
	if (hs->cleaner_id != 0) {
		hs->cleaner_stop = 1;

		if ((tret = pthread_join(hs->cleaner_id, NULL)) != 0)
			EMSG(wtext, session, tret,
			    "pthread_join: %s", strerror(tret));
		hs->cleaner_id = 0;
	}

	/* Close the underlying WiredTiger sources. */
	while ((ws = hs->ws_head) != NULL) {
		hs->ws_head = ws->next;
		ESET(ws_source_close(wtext, session, ws));
	}

	/* If the owner, close the database transaction store. */
	if (hs->he_txn != NULL && hs->he_owner) {
		if ((tret = he_close(hs->he_txn)) != 0)
			EMSG(wtext, session, tret,
			    "he_close: %s: %s: %s",
			    hs->name, WT_NAME_TXN, he_strerror(tret));
		hs->he_txn = NULL;
	}

	/* Flush and close the Helium source. */
	if (hs->he_volume != NULL) {
		if ((tret = he_commit(hs->he_volume)) != 0)
			EMSG(wtext, session, tret,
			    "he_commit: %s: %s",
			    hs->device, he_strerror(tret));

		if ((tret = he_close(hs->he_volume)) != 0)
			EMSG(wtext, session, tret,
			    "he_close: %s: %s: %s",
			    hs->name, WT_NAME_INIT, he_strerror(tret));
		hs->he_volume = NULL;
	}

	free(hs->name);
	free(hs->device);
	OVERWRITE_AND_FREE(hs);

	return (ret);
}

/*
 * cache_cleaner --
 *	Migrate information from the cache to the primary store.
 */
static int
cache_cleaner(WT_EXTENSION_API *wtext,
    WT_CURSOR *wtcursor, uint64_t oldest, uint64_t *txnminp)
{
	CACHE_RECORD *cp;
	CURSOR *cursor;
	HE_ITEM *r;
	WT_SOURCE *ws;
	uint64_t txnid;
	int locked, pushed, recovery, ret = 0;

	/*
	 * Called in two ways: in normal processing mode where we're supplied a
	 * value for the oldest transaction ID not yet visible to a running
	 * transaction, and we're tracking the smallest transaction ID
	 * referenced by any cache entry, and in recovery mode where neither of
	 * those are true.
	 */
	if (txnminp == NULL)
		recovery = 1;
	else {
		recovery = 0;
		*txnminp = UINT64_MAX;
	}

	cursor = (CURSOR *)wtcursor;
	ws = cursor->ws;
	r = &cursor->record;
	locked = pushed = 0;

	/*
	 * For every cache key where all updates are globally visible:
	 *	Migrate the most recent update value to the primary store.
	 */
	for (r->key_len = 0; (ret =
	    helium_call(wtcursor, "he_next", ws->he_cache, he_next)) == 0;) {
		/*
		 * Unmarshall the value, and if all of the updates are globally
		 * visible, update the primary with the last committed update.
		 * In normal processing, the last committed update test is for
		 * a globally visible update that's not explicitly aborted.  In
		 * recovery processing, the last committed update test is for
		 * an explicitly committed update.  See the underlying functions
		 * for more information.
		 */
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			goto err;
		if (!recovery && !cache_value_visible_all(wtcursor, oldest))
			continue;
		if (recovery)
			cache_value_last_committed(wtcursor, &cp);
		else
			cache_value_last_not_aborted(wtcursor, &cp);
		if (cp == NULL)
			continue;

		pushed = 1;
		if (cp->remove) {
			if ((ret = he_delete(ws->he, r)) == 0)
				continue;

			/*
			 * Updates confined to the cache may not appear in the
			 * primary at all, that is, an insert and remove pair
			 * may be confined to the cache.
			 */
			if (ret == HE_ERR_ITEM_NOT_FOUND) {
				ret = 0;
				continue;
			}
			ERET(wtext, NULL, ret,
			    "he_delete: %s", he_strerror(ret));
		} else {
			r->val = cp->v;
			r->val_len = cp->len;
			/*
			 * If compression configured for this datastore, set the
			 * compression flag, we're updating the "real" store.
			 */
			if (ws->config_compress)
				r->flags |= HE_I_COMPRESS;
			ret = he_update(ws->he, r);
			r->flags = 0;
			if (ret == 0)
				continue;

			ERET(wtext, NULL, ret,
			    "he_update: %s", he_strerror(ret));
		}
	}

	if (ret == WT_NOTFOUND)
		ret = 0;
	if (ret != 0)
		ERET(wtext, NULL, ret, "he_next: %s", he_strerror(ret));

	/*
	 * If we didn't move any keys from the cache to the primary, quit.  It's
	 * possible we could still remove values from the cache, but not likely,
	 * and another pass would probably be wasted effort (especially locked).
	 */
	if (!pushed)
		return (0);

	/*
	 * Push the store to stable storage for correctness.  (It doesn't matter
	 * what Helium handle we commit, so we just commit one of them.)
	 */
	if ((ret = he_commit(ws->he)) != 0)
		ERET(wtext, NULL, ret, "he_commit: %s", he_strerror(ret));

	/*
	 * If we're performing recovery, that's all we need to do, we're going
	 * to simply discard the cache, there's no reason to remove entries one
	 * at a time.
	 */
	if (recovery)
		return (0);

	/*
	 * For every cache key where all updates are globally visible:
	 *	Remove the cache key.
	 *
	 * We're updating the cache, which requires a lock during normal
	 * cleaning.
	 */
	if ((ret = writelock(wtext, NULL, &ws->lock)) != 0)
		goto err;
	locked = 1;

	for (r->key_len = 0; (ret =
	    helium_call(wtcursor, "he_next", ws->he_cache, he_next)) == 0;) {
		/*
		 * Unmarshall the value, and if all of the updates are globally
		 * visible, remove the cache entry.
		 */
		if ((ret = cache_value_unmarshall(wtcursor)) != 0)
			goto err;
		if (cache_value_visible_all(wtcursor, oldest)) {
			if ((ret = he_delete(ws->he_cache, r)) != 0)
				EMSG_ERR(wtext, NULL, ret,
				    "he_delete: %s", he_strerror(ret));
			continue;
		}

		/*
		 * If the entry will remain in the cache, figure out the oldest
		 * transaction for which it contains an update (which might be
		 * different from the oldest transaction in the system).  We
		 * need the oldest transaction ID that appears anywhere in any
		 * cache, it limits the records we can discard from the
		 * transaction store.
		 */
		cache_value_txnmin(wtcursor, &txnid);
		if (txnid < *txnminp)
			*txnminp = txnid;
	}

	locked = 0;
	if ((ret = unlock(wtext, NULL, &ws->lock)) != 0)
		goto err;
	if (ret == WT_NOTFOUND)
		ret = 0;
	if (ret != 0)
		EMSG_ERR(wtext, NULL, ret, "he_next: %s", he_strerror(ret));

err:	if (locked)
		ESET(unlock(wtext, NULL, &ws->lock));

	return (ret);
}

/*
 * txn_cleaner --
 *	Discard no longer needed entries from the transaction store.
 */
static int
txn_cleaner(WT_CURSOR *wtcursor, he_t he_txn, uint64_t txnmin)
{
	CURSOR *cursor;
	HE_ITEM *r;
	WT_EXTENSION_API *wtext;
	uint64_t txnid;
	int ret = 0;

	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	r = &cursor->record;

	/*
	 * Remove all entries from the transaction store that are before the
	 * oldest transaction ID that appears anywhere in any cache.
	 */
	for (r->key_len = 0;
	    (ret = helium_call(wtcursor, "he_next", he_txn, he_next)) == 0;) {
		memcpy(&txnid, r->key, sizeof(txnid));
		if (txnid < txnmin && (ret = he_delete(he_txn, r)) != 0)
			ERET(wtext, NULL, ret,
			    "he_delete: %s", he_strerror(ret));
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	if (ret != 0)
		ERET(wtext, NULL, ret, "he_next: %s", he_strerror(ret));

	return (0);
}

/*
 * fake_cursor --
 *	Fake up enough of a cursor to do Helium operations.
 */
static int
fake_cursor(WT_EXTENSION_API *wtext, WT_CURSOR **wtcursorp)
{
	CURSOR *cursor;
	WT_CURSOR *wtcursor;

	/*
	 * Fake a cursor.
	 */
	if ((cursor = calloc(1, sizeof(CURSOR))) == NULL)
		return (os_errno());
	cursor->wtext = wtext;
	cursor->record.key = cursor->__key;
	if ((cursor->v = malloc(128)) == NULL) {
		free(cursor);
		return (os_errno());
	}
	cursor->mem_len = 128;

	/*
	 * !!!
	 * Fake cursors don't have WT_SESSION handles.
	 */
	wtcursor = (WT_CURSOR *)cursor;
	wtcursor->session = NULL;

	*wtcursorp = wtcursor;
	return (0);
}

/*
 * cache_cleaner_worker --
 *	Thread to migrate data from the cache to the primary.
 */
static void *
cache_cleaner_worker(void *arg)
{
	struct timeval t;
	CURSOR *cursor;
	HELIUM_SOURCE *hs;
	HE_STATS stats;
	WT_CURSOR *wtcursor;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	uint64_t oldest, txnmin, txntmp;
	int cleaner_stop, delay, ret = 0;

	hs = (HELIUM_SOURCE *)arg;

	cursor = NULL;
	wtext = hs->wtext;

	if ((ret = fake_cursor(wtext, &wtcursor)) != 0)
		EMSG_ERR(wtext, NULL, ret, "cleaner: %s", strerror(ret));
	cursor = (CURSOR *)wtcursor;

	for (cleaner_stop = delay = 0; !cleaner_stop;) {
		/*
		 * Check if this will be the final run; cleaner_stop is declared
		 * volatile, and so the read will happen.  We don't much care if
		 * there's extra loops, it's enough if a read eventually happens
		 * and finds the variable set.  Store the read locally, reading
		 * the variable twice might race.
		 */
		cleaner_stop = hs->cleaner_stop;

		/*
		 * Delay if this isn't the final run and the last pass didn't
		 * find any work to do.
		 */
		if (!cleaner_stop && delay != 0) {
			t.tv_sec = delay;
			t.tv_usec = 0;
			(void)select(0, NULL, NULL, NULL, &t);
		}

		/* Run at least every 5 seconds. */
		if (delay < 5)
			++delay;

		/*
		 * Clean the datastore caches, depending on their size.  It's
		 * both more and less expensive to return values from the cache:
		 * more because we have to marshall/unmarshall the values, less
		 * because there's only a single call, to the cache store rather
		 * one to the cache and one to the primary.  I have no turning
		 * information, for now simply set the limit at 50MB.
		 */
#undef	CACHE_SIZE_TRIGGER
#define	CACHE_SIZE_TRIGGER	(50 * 1048576)
		for (ws = hs->ws_head; ws != NULL; ws = ws->next) {
			if ((ret = he_stats(ws->he_cache, &stats)) != 0)
				EMSG_ERR(wtext, NULL,
				    ret, "he_stats: %s", he_strerror(ret));
			if (stats.size > CACHE_SIZE_TRIGGER)
				break;
		}
		if (!cleaner_stop && ws == NULL)
			continue;

		/* There was work to do, don't delay before checking again. */
		delay = 0;

		/*
		 * Get the oldest transaction ID not yet visible to a running
		 * transaction.  Do this before doing anything else, avoiding
		 * any race with creating new WT_SOURCE handles.
		 */
		oldest = wtext->transaction_oldest(wtext);

		/*
		 * If any cache needs cleaning, clean them all, because we have
		 * to know the minimum transaction ID referenced by any cache.
		 *
		 * For each cache/primary pair, migrate whatever records we can,
		 * tracking the lowest transaction ID of any entry in any cache.
		 */
		txnmin = UINT64_MAX;
		for (ws = hs->ws_head; ws != NULL; ws = ws->next) {
			cursor->ws = ws;
			if ((ret = cache_cleaner(
			    wtext, wtcursor, oldest, &txntmp)) != 0)
				goto err;
			if (txntmp < txnmin)
				txnmin = txntmp;
		}

		/*
		 * Discard any transactions less than the minimum transaction ID
		 * referenced in any cache.
		 *
		 * !!!
		 * I'm playing fast-and-loose with whether or not the cursor
		 * references an underlying WT_SOURCE, there's a structural
		 * problem here.
		 */
		cursor->ws = NULL;
		if ((ret = txn_cleaner(wtcursor, hs->he_txn, txnmin)) != 0)
			goto err;
	}

err:	cursor_destroy(cursor);
	return (NULL);
}

/*
 * helium_config_read --
 *	Parse the Helium configuration.
 */
static int
helium_config_read(WT_EXTENSION_API *wtext, WT_CONFIG_ITEM *config,
    char **devicep, HE_ENV *envp, int *env_setp, int *flagsp)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *config_parser;
	int ret = 0, tret;

	*env_setp = 0;
	*flagsp = 0;

	/* Traverse the configuration arguments list. */
	if ((ret = wtext->config_parser_open(
	    wtext, NULL, config->str, config->len, &config_parser)) != 0)
		ERET(wtext, NULL, ret,
		    "WT_EXTENSION_API.config_parser_open: %s",
		    wtext->strerror(wtext, NULL, ret));
	while ((ret = config_parser->next(config_parser, &k, &v)) == 0) {
		if (string_match("helium_devices", k.str, k.len)) {
			if ((*devicep = calloc(1, v.len + 1)) == NULL)
				return (os_errno());
			memcpy(*devicep, v.str, v.len);
			continue;
		}
		if (string_match("helium_env_read_cache_size", k.str, k.len)) {
			envp->read_cache_size = (uint64_t)v.val;
			*env_setp = 1;
			continue;
		}
		if (string_match("helium_env_write_cache_size", k.str, k.len)) {
			envp->write_cache_size = (uint64_t)v.val;
			*env_setp = 1;
			continue;
		}
		if (string_match("helium_o_volume_truncate", k.str, k.len)) {
			if (v.val != 0)
				*flagsp |= HE_O_VOLUME_TRUNCATE;
			continue;
		}
		EMSG_ERR(wtext, NULL, EINVAL,
		    "unknown configuration key value pair %.*s=%.*s",
		    (int)k.len, k.str, (int)v.len, v.str);
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	if (ret != 0)
		EMSG_ERR(wtext, NULL, ret,
		    "WT_CONFIG_PARSER.next: %s",
		    wtext->strerror(wtext, NULL, ret));

err:	if ((tret = config_parser->close(config_parser)) != 0)
		EMSG(wtext, NULL, tret,
		    "WT_CONFIG_PARSER.close: %s",
		    wtext->strerror(wtext, NULL, tret));

	return (ret);
}

/*
 * helium_source_open --
 *	Allocate and open a Helium source.
 */
static int
helium_source_open(DATA_SOURCE *ds, WT_CONFIG_ITEM *k, WT_CONFIG_ITEM *v)
{
	struct he_env env;
	HELIUM_SOURCE *hs;
	WT_EXTENSION_API *wtext;
	int env_set, flags, ret = 0;

	wtext = ds->wtext;
	hs = NULL;

	VMSG(wtext, NULL, VERBOSE_L1, "volume %.*s=%.*s",
	    (int)k->len, k->str, (int)v->len, v->str);

	/*
	 * Check for a Helium source we've already opened: we don't check the
	 * value (which implies you can open the same underlying stores using
	 * more than one name, but I don't know of any problems that causes),
	 * we only check the key, that is, the top-level WiredTiger name.
	 */
	for (hs = ds->hs_head; hs != NULL; hs = hs->next)
		if (string_match(hs->name, k->str, k->len))
			ERET(wtext, NULL,
			    EINVAL, "%s: device already open", hs->name);

	/* Allocate and initialize a new underlying Helium source object. */
	if ((hs = calloc(1, sizeof(*hs))) == NULL ||
	    (hs->name = calloc(1, k->len + 1)) == NULL) {
		free(hs);
		return (os_errno());
	}
	memcpy(hs->name, k->str, k->len);
	hs->txn_notify.notify = txn_notify;
	hs->wtext = wtext;

	/* Read the configuration, require a device naming the Helium store. */
	memset(&env, 0, sizeof(env));
	if ((ret = helium_config_read(
	    wtext, v, &hs->device, &env, &env_set, &flags)) != 0)
		goto err;
	if (hs->device == NULL)
		EMSG_ERR(wtext, NULL,
		    EINVAL, "%s: no Helium volumes specified", hs->name);

	/*
	 * Open the Helium volume, creating it if necessary.  We have to open
	 * an object at the same time, that's why we have object flags as well
	 * as volume flags.
	 */
	flags |= HE_O_CREATE |
	    HE_O_TRUNCATE | HE_O_VOLUME_CLEAN | HE_O_VOLUME_CREATE;
	if ((hs->he_volume = he_open(
	    hs->device, WT_NAME_INIT, flags, env_set ? &env : NULL)) == NULL) {
		ret = os_errno();
		EMSG_ERR(wtext, NULL, ret,
		    "he_open: %s: %s: %s",
		    hs->name, WT_NAME_INIT, he_strerror(ret));
	}

	/* Insert the new entry at the head of the list. */
	hs->next = ds->hs_head;
	ds->hs_head = hs;

	if (0) {
err:		if (hs != NULL)
			ESET(helium_source_close(wtext, NULL, hs));
	}
	return (ret);
}

/*
 * helium_source_open_txn --
 *	Open the database-wide transaction store.
 */
static int
helium_source_open_txn(DATA_SOURCE *ds)
{
	HELIUM_SOURCE *hs, *hs_txn;
	WT_EXTENSION_API *wtext;
	he_t he_txn, t;
	int ret = 0;

	wtext = ds->wtext;

	/*
	 * The global txn namespace is per connection, it spans multiple Helium
	 * sources.
	 *
	 * We've opened the Helium sources: check to see if any of them already
	 * have a transaction store, and make sure we only find one.
	 */
	hs_txn = NULL;
	he_txn = NULL;
	for (hs = ds->hs_head; hs != NULL; hs = hs->next)
		if ((t = he_open(hs->device, WT_NAME_TXN, 0, NULL)) != NULL) {
			if (hs_txn != NULL) {
				(void)he_close(t);
				(void)he_close(hs_txn);
				ERET(wtext, NULL, WT_PANIC,
				    "found multiple transaction stores, "
				    "unable to proceed");
			}
			he_txn = t;
			hs_txn = hs;
		}

	/*
	 * If we didn't find a transaction store, open a transaction store in
	 * the first Helium source we loaded. (It could just as easily be the
	 * last one we loaded, we're just picking one, but picking the first
	 * seems slightly less likely to make people wonder.)
	 */
	if ((hs = hs_txn) == NULL) {
		for (hs = ds->hs_head; hs->next != NULL; hs = hs->next)
			;
		if ((he_txn = he_open(
		    hs->device, WT_NAME_TXN, HE_O_CREATE, NULL)) == NULL) {
			ret = os_errno();
			ERET(wtext, NULL, ret,
			    "he_open: %s: %s: %s",
			    hs->name, WT_NAME_TXN, he_strerror(ret));
		}

		/* Push the change. */
		if ((ret = he_commit(he_txn)) != 0)
			ERET(wtext, NULL, ret,
			    "he_commit: %s", he_strerror(ret));
	}
	VMSG(wtext, NULL, VERBOSE_L1, "%s" "transactional store on %s",
	    hs_txn == NULL ? "creating " : "", hs->name);

	/* Set the owner field, this Helium source has to be closed last. */
	hs->he_owner = 1;

	/* Add a reference to the transaction store in each Helium source. */
	for (hs = ds->hs_head; hs != NULL; hs = hs->next)
		hs->he_txn = he_txn;

	return (0);
}

/*
 * helium_source_recover_namespace --
 *	Recover a single cache/primary pair in a Helium namespace.
 */
static int
helium_source_recover_namespace(WT_DATA_SOURCE *wtds,
    HELIUM_SOURCE *hs, const char *name, WT_CONFIG_ARG *config)
{
	CURSOR *cursor;
	DATA_SOURCE *ds;
	WT_CURSOR *wtcursor;
	WT_EXTENSION_API *wtext;
	WT_SOURCE *ws;
	size_t len;
	int ret = 0;
	const char *p;
	char *uri;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	cursor = NULL;
	ws = NULL;
	uri = NULL;

	/*
	 * The name we store on the Helium device is a translation of the
	 * WiredTiger name: do the reverse process here so we can use the
	 * standard source-open function.
	 */
	p = name + strlen(WT_NAME_PREFIX);
	len = strlen("helium:") + strlen(hs->name) + strlen(p) + 10;
	if ((uri = malloc(len)) == NULL) {
		ret = os_errno();
		goto err;
	}
	(void)snprintf(uri, len, "helium:%s/%s", hs->name, p);

	/*
	 * Open the cache/primary pair by going through the full open process,
	 * instantiating the underlying WT_SOURCE object.
	 */
	if ((ret = ws_source_open(wtds, NULL, uri, config, 0, &ws)) != 0)
		goto err;
	if ((ret = unlock(wtext, NULL, &ws->lock)) != 0)
		goto err;

	/* Fake up a cursor. */
	if ((ret = fake_cursor(wtext, &wtcursor)) != 0)
		EMSG_ERR(wtext, NULL, ret, "recovery: %s", strerror(ret));
	cursor = (CURSOR *)wtcursor;
	cursor->ws = ws;

	/* Process, then clear, the cache. */
	if ((ret = cache_cleaner(wtext, wtcursor, 0, NULL)) != 0)
		goto err;
	if ((ret = he_truncate(ws->he_cache)) != 0)
		EMSG_ERR(wtext, NULL, ret,
		    "he_truncate: %s(cache): %s", ws->uri, he_strerror(ret));

	/* Close the underlying WiredTiger sources. */
err:	while ((ws = hs->ws_head) != NULL) {
		hs->ws_head = ws->next;
		ESET(ws_source_close(wtext, NULL, ws));
	}

	cursor_destroy(cursor);
	free(uri);

	return (ret);
}

struct helium_namespace_cookie {
	char **list;
	u_int  list_cnt;
	u_int  list_max;
};

/*
 * helium_namespace_list --
 *	Get a list of the objects we're going to recover.
 */
static int
helium_namespace_list(void *cookie, const char *name)
{
	struct helium_namespace_cookie *names;
	void *allocp;

	names = cookie;

	/*
	 * Ignore any files without a WiredTiger prefix.
	 * Ignore the metadata and cache files.
	 */
	if (!prefix_match(name, WT_NAME_PREFIX))
		return (0);
	if (strcmp(name, WT_NAME_INIT) == 0)
		return (0);
	if (strcmp(name, WT_NAME_TXN) == 0)
		return (0);
	if (string_match(
	    strrchr(name, '.'), WT_NAME_CACHE, strlen(WT_NAME_CACHE)))
		return (0);

	if (names->list_cnt + 1 >= names->list_max) {
		if ((allocp = realloc(names->list,
		    (names->list_max + 20) * sizeof(names->list[0]))) == NULL)
			return (os_errno());
		names->list = allocp;
		names->list_max += 20;
	}
	if ((names->list[names->list_cnt] = strdup(name)) == NULL)
		return (os_errno());
	++names->list_cnt;
	names->list[names->list_cnt] = NULL;
	return (0);
}

/*
 * helium_source_recover --
 *	Recover the HELIUM_SOURCE.
 */
static int
helium_source_recover(
    WT_DATA_SOURCE *wtds, HELIUM_SOURCE *hs, WT_CONFIG_ARG *config)
{
	struct helium_namespace_cookie names;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	u_int i;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	memset(&names, 0, sizeof(names));

	VMSG(wtext, NULL, VERBOSE_L1, "recover %s", hs->name);

	/* Get a list of the cache/primary object pairs in the Helium source. */
	if ((ret = he_enumerate(
	    hs->device, helium_namespace_list, &names)) != 0)
		ERET(wtext, NULL, ret,
		    "he_enumerate: %s: %s", hs->name, he_strerror(ret));

	/* Recover the objects. */
	for (i = 0; i < names.list_cnt; ++i)
		if ((ret = helium_source_recover_namespace(
		    wtds, hs, names.list[i], config)) != 0)
			goto err;

	/* Clear the transaction store. */
	if ((ret = he_truncate(hs->he_txn)) != 0)
		EMSG_ERR(wtext, NULL, ret,
		    "he_truncate: %s: %s: %s",
		    hs->name, WT_NAME_TXN, he_strerror(ret));

err:	for (i = 0; i < names.list_cnt; ++i)
		free(names.list[i]);
	free(names.list);

	return (ret);
}

/*
 * helium_terminate --
 *	Unload the data-source.
 */
static int
helium_terminate(WT_DATA_SOURCE *wtds, WT_SESSION *session)
{
	DATA_SOURCE *ds;
	HELIUM_SOURCE *hs, *last;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Lock the system down. */
	if (ds->lockinit)
		ret = writelock(wtext, session, &ds->global_lock);

	/*
	 * Close the Helium sources, close the Helium source that "owns" the
	 * database transaction store last.
	 */
	last = NULL;
	while ((hs = ds->hs_head) != NULL) {
		ds->hs_head = hs->next;
		if (hs->he_owner) {
			last = hs;
			continue;
		}
		ESET(helium_source_close(wtext, session, hs));
	}
	if (last != NULL)
		ESET(helium_source_close(wtext, session, last));

	/* Unlock and destroy the system. */
	if (ds->lockinit) {
		ESET(unlock(wtext, session, &ds->global_lock));
		ESET(lock_destroy(wtext, NULL, &ds->global_lock));
	}

	OVERWRITE_AND_FREE(ds);

	return (ret);
}

/*
 * wiredtiger_extension_init --
 *	Initialize the Helium connector code.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	/*
	 * List of the WT_DATA_SOURCE methods -- it's static so it breaks at
	 * compile-time should the structure change underneath us.
	 */
	static const WT_DATA_SOURCE wtds = {
		helium_session_create,		/* session.create */
		NULL,				/* No session.compaction */
		helium_session_drop,		/* session.drop */
		helium_session_open_cursor,	/* session.open_cursor */
		helium_session_rename,		/* session.rename */
		NULL,				/* No session.salvage */
		helium_session_truncate,	/* session.truncate */
		NULL,				/* No session.range_truncate */
		helium_session_verify,		/* session.verify */
		helium_session_checkpoint,	/* session.checkpoint */
		helium_terminate		/* termination */
	};
	static const char *session_create_opts[] = {
		"helium_o_compress=0",		/* HE_I_COMPRESS */
		"helium_o_truncate=0",		/* HE_O_TRUNCATE */
		NULL
	};
	DATA_SOURCE *ds;
	HELIUM_SOURCE *hs;
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *config_parser;
	WT_EXTENSION_API *wtext;
	int vmajor, vminor, ret = 0;
	const char **p;

	config_parser = NULL;
	ds = NULL;

	wtext = connection->get_extension_api(connection);

						/* Check the library version */
#if HE_VERSION_MAJOR != 2 || HE_VERSION_MINOR != 2
	ERET(wtext, NULL, EINVAL,
	    "unsupported Levyx/Helium header file %d.%d, expected version 2.2",
	    HE_VERSION_MAJOR, HE_VERSION_MINOR);
#endif
	he_version(&vmajor, &vminor);
	if (vmajor != 2 || vminor != 2)
		ERET(wtext, NULL, EINVAL,
		    "unsupported Levyx/Helium library version %d.%d, expected "
		    "version 2.2", vmajor, vminor);

	/* Allocate and initialize the local data-source structure. */
	if ((ds = calloc(1, sizeof(DATA_SOURCE))) == NULL)
		return (os_errno());
	ds->wtds = wtds;
	ds->wtext = wtext;
	if ((ret = lock_init(wtext, NULL, &ds->global_lock)) != 0)
		goto err;
	ds->lockinit = 1;

	/* Get the configuration string. */
	if ((ret = wtext->config_get(wtext, NULL, config, "config", &v)) != 0)
		EMSG_ERR(wtext, NULL, ret,
		    "WT_EXTENSION_API.config_get: config: %s",
		    wtext->strerror(wtext, NULL, ret));

	/* Step through the list of Helium sources, opening each one. */
	if ((ret = wtext->config_parser_open(
	    wtext, NULL, v.str, v.len, &config_parser)) != 0)
		EMSG_ERR(wtext, NULL, ret,
		    "WT_EXTENSION_API.config_parser_open: config: %s",
		    wtext->strerror(wtext, NULL, ret));
	while ((ret = config_parser->next(config_parser, &k, &v)) == 0) {
		if (string_match("helium_verbose", k.str, k.len)) {
			verbose = v.val == 0 ? 0 : 1;
			continue;
		}
		if ((ret = helium_source_open(ds, &k, &v)) != 0)
			goto err;
	}
	if (ret != WT_NOTFOUND)
		EMSG_ERR(wtext, NULL, ret,
		    "WT_CONFIG_PARSER.next: config: %s",
		    wtext->strerror(wtext, NULL, ret));
	if ((ret = config_parser->close(config_parser)) != 0)
		EMSG_ERR(wtext, NULL, ret,
		    "WT_CONFIG_PARSER.close: config: %s",
		    wtext->strerror(wtext, NULL, ret));
	config_parser = NULL;

	/* Find and open the database transaction store. */
	if ((ret = helium_source_open_txn(ds)) != 0)
		return (ret);

	/* Recover each Helium source. */
	for (hs = ds->hs_head; hs != NULL; hs = hs->next)
		if ((ret = helium_source_recover(&ds->wtds, hs, config)) != 0)
			goto err;

	/* Start each Helium source cleaner thread. */
	for (hs = ds->hs_head; hs != NULL; hs = hs->next)
		if ((ret = pthread_create(
		    &hs->cleaner_id, NULL, cache_cleaner_worker, hs)) != 0)
			EMSG_ERR(wtext, NULL, ret,
			    "%s: pthread_create: cleaner thread: %s",
			    hs->name, strerror(ret));

	/* Add Helium-specific WT_SESSION.create configuration options.  */
	for (p = session_create_opts; *p != NULL; ++p)
		if ((ret = connection->configure_method(connection,
		    "WT_SESSION.create", "helium:", *p, "boolean", NULL)) != 0)
			EMSG_ERR(wtext, NULL, ret,
			    "WT_CONNECTION.configure_method: session.create: "
			    "%s: %s",
			    *p, wtext->strerror(wtext, NULL, ret));

	/* Add the data source */
	if ((ret = connection->add_data_source(
	    connection, "helium:", (WT_DATA_SOURCE *)ds, NULL)) != 0)
		EMSG_ERR(wtext, NULL, ret,
		    "WT_CONNECTION.add_data_source: %s",
		    wtext->strerror(wtext, NULL, ret));
	return (0);

err:	if (ds != NULL)
		ESET(helium_terminate((WT_DATA_SOURCE *)ds, NULL));
	if (config_parser != NULL)
		(void)config_parser->close(config_parser);
	return (ret);
}

/*
 * wiredtiger_extension_terminate --
 *	Shutdown the Helium connector code.
 */
int
wiredtiger_extension_terminate(WT_CONNECTION *connection)
{
	(void)connection;			/* Unused parameters */

	return (0);
}
