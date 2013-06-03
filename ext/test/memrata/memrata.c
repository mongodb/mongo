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

/*
 * I think it's pretty unlikely we'll ever need to revise the version, but
 * it can't hurt to be cautious.
 */
#define	KVS_MAJOR	1			/* KVS major, minor version */
#define	KVS_MINOR	0

/*
 * We partition the flat space into a set of objects based on a packed, 8B
 * leading byte.
 *
 * Object ID 1 is for master records; object ID from 1 to KVS_MAXID_BASE are
 * reserved for future use (and, I suppose object ID 0 would work as well).
 * I can't think of any reason I'd need them, but it's a cheap and easy backup
 * plan.
 */
#define	KVS_MASTER_ID		1		/* Master object ID */
#define	KVS_MASTER_VALUE_MAX	256		/* Maximum master value */

#define	KVS_MAXID	"WiredTiger.maxid"	/* Maximum object ID key */
#define	KVS_MAXID_BASE	6			/* First object ID */

/*
 * Each KVS source supports a set of URIs (named objects).  Cursors reference
 * their underlying URI and underlying KVS source.
 */
typedef struct __uri_source {
	char *name;				/* Unique name */
	pthread_rwlock_t lock;			/* Lock */

	int	configured;			/* If configured */
	u_int	ref;				/* Active reference count */

	/*
	 * Each object has a unique leading byte prefix, which is the object's
	 * ID turned into a packed string (any 8B unsigned value will pack
	 * into a maximum of 9 bytes.)  We create a packed copy of the object's
	 * ID and a packed copy of the ID one greateer than the object's ID,
	 * the latter is what we use for a "previous" cursor traversal.
	 */
#define	KVS_MAX_PACKED_8B	10
	char	 id[KVS_MAX_PACKED_8B];		/* Packed unique ID prefix */
	size_t	 idlen;				/* ID prefix length */
	char	 idnext[KVS_MAX_PACKED_8B];	/* Packed next ID prefix */
	size_t	 idnextlen;			/* Next ID prefix length */

	uint64_t append_recno;			/* Allocation record number */

	int	 config_recno;			/* config "key_format=r" */
	int	 config_bitfield;		/* config "value_format=#t" */

	struct __kvs_source *ks;		/* Underlying KVS source */

	struct __uri_source *next;		/* List of URIs */
} URI_SOURCE;

typedef struct __kvs_source {
	char *name;				/* Unique name */
	kvs_t kvs;				/* Underlying KVS reference */

	struct __uri_source *uri_head;		/* List of URIs */

	struct __kvs_source *next;		/* List of KVS sources */
} KVS_SOURCE;

typedef struct __cursor {
	WT_CURSOR wtcursor;			/* Must come first */

	WT_EXTENSION_API *wtext;		/* Extension functions */

	KVS_SOURCE *ks;				/* Underlying KVS source */
	URI_SOURCE *us;				/* Underlying URI */

	struct kvs_record record;		/* Record */
	char   key[KVS_MAX_KEY_LEN];		/* key, value */
	char  *val;
	size_t val_len;

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
copyin_key(WT_CURSOR *wtcursor)
{
	struct kvs_record *r;
	CURSOR *cursor;
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	size_t i, size;
	uint8_t *p, *t;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	us = cursor->us;
	wtext = cursor->wtext;

	r = &cursor->record;
	r->key = cursor->key;
	r->key_len = 0;

	/* Prefix the key with the object's unique ID. */
	for (p = us->id, t = r->key, i = 0; i < us->idlen; ++i)
		*t++ = *p++;
	r->key_len += us->idlen;

	if (us->config_recno) {
		if ((ret = wtext->struct_size(wtext, session,
		    &size, "r", wtcursor->recno)) != 0 ||
		    (ret = wtext->struct_pack(wtext, session, t,
		    sizeof(r->key) - us->idlen, "r", wtcursor->recno)) != 0)
			return (ret);
		r->key_len += size;

		/*
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
		if (wtcursor->recno > us->append_recno) {
			if ((ret = writelock(wtext, session, &us->lock)) != 0)
				return (ret);
			if (wtcursor->recno > us->append_recno)
				us->append_recno = wtcursor->recno;
			if ((ret = unlock(wtext, session, &us->lock)) != 0)
				return (ret);
		}
	} else {
		if (wtcursor->key.size + us->idlen > KVS_MAX_KEY_LEN)
			ERET(wtext, session, ERANGE,
			    "key size of %" PRIuMAX " is too large",
			    (uintmax_t)wtcursor->key.size);
		memcpy(t, wtcursor->key.data, wtcursor->key.size);
		r->key_len += wtcursor->key.size;
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
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	size_t i, len;
	uint8_t *p, *t;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	us = cursor->us;
	wtext = cursor->wtext;

	r = &cursor->record;

	/*
	 * Check the object's unique ID, if it doesn't match we've hit the end
	 * of the object on a cursor search.
	 */
	for (p = us->id, t = r->key, i = 0; i < us->idlen; ++i)
		if (*t++ != *p++)
			return (WT_NOTFOUND);
	len = r->key_len - us->idlen;

	if (us->config_recno) {
		if ((ret = wtext->struct_unpack(
		    wtext, session, t, len, "r", &wtcursor->recno)) != 0)
			return (ret);
	} else {
		wtcursor->key.data = t;
		wtcursor->key.size = (uint32_t)len;
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
 * kvs_dump --
 *	Dump the records in the KVS store.
 */
static void
kvs_dump(
    WT_EXTENSION_API *wtext, WT_SESSION *session, kvs_t kvs, const char *tag)
{
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	uint64_t recno;
	size_t len, size;
	char *p, key[256], val[256];

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
		    (int)len - size, p + size, (int)r->val_len, r->val);

		r->val_len = sizeof(val);
	}
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
 * nextprev_init --
 *	Initialize a cursor for a next/previous traversal.
 */
static INLINE void
nextprev_init(WT_CURSOR *wtcursor, uint8_t *id, size_t idlen)
{
	CURSOR *cursor;
	size_t i;
	uint8_t *p, *t;

	cursor = (CURSOR *)wtcursor;

	/* Prefix the key with a unique ID. */
	for (p = id, t = cursor->key, i = 0; i < idlen; ++i)
		*t++ = *p++;

	cursor->record.key = cursor->key;
	cursor->record.key_len = idlen;
}

/*
 * kvs_cursor_next --
 *	WT_CURSOR::next method.
 */
static int
kvs_cursor_next(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	URI_SOURCE *us;
	int ret = 0;

	cursor = (CURSOR *)wtcursor;
	us = cursor->us;

	/*
	 * If this is the start of a new traversal, set the key to the first
	 * possible record for the object.
	 */
	if (cursor->record.key_len == 0)
		nextprev_init(wtcursor, us->id, us->idlen);

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
	CURSOR *cursor;
	URI_SOURCE *us;
	int ret = 0;

	cursor = (CURSOR *)wtcursor;
	us = cursor->us;

	/*
	 * If this is the start of a new traversal, set the key to the first
	 * possible record after the object.
	 */
	if (cursor->record.key_len == 0)
		nextprev_init(wtcursor, us->idnext, us->idnextlen);

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
 * kvs_cursor_insert --
 *	WT_CURSOR::insert method.
 */
static int
kvs_cursor_insert(WT_CURSOR *wtcursor)
{
	CURSOR *cursor;
	KVS_SOURCE *ks;
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ks;
	us = cursor->us;

	/* Allocate a new record for append operations. */
	if (cursor->config_append) {
		if ((ret = writelock(wtext, session, &us->lock)) != 0)
			return (ret);
		wtcursor->recno = ++us->append_recno;
		if ((ret = unlock(wtext, session, &us->lock)) != 0)
			return (ret);
	}

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
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	ks = cursor->ks;
	us = cursor->us;

	/*
	 * WiredTiger's "remove" of a bitfield is really an update with a value
	 * of a single byte of zero.
	 */
	if (us->config_bitfield) {
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
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	WT_SESSION *session;
	int ret = 0;

	session = wtcursor->session;
	cursor = (CURSOR *)wtcursor;
	wtext = cursor->wtext;
	us = cursor->us;

	if ((ret = writelock(wtext, session, &us->lock)) != 0)
		goto err;
	--us->ref;
	if ((ret = unlock(wtext, session, &us->lock)) != 0)
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

#define	KVS_FLAG_SET(n, f)						\
		if (strcmp(name, n) == 0) {				\
			if (v.val != 0)					\
				*flagsp |= f;				\
			continue;					\
		}
		KVS_FLAG_SET("kvs_open_o_debug", KVS_O_DEBUG);
		KVS_FLAG_SET("kvs_open_o_reclaim", KVS_O_SCAN);
		KVS_FLAG_SET("kvs_open_o_scan",  KVS_O_RECLAIM);
		KVS_FLAG_SET("kvs_open_o_truncate",  KVS_O_CREATE);
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
		return (__os_errno());

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
	 * Open the KVS handle last, so cleanup is easier: we don't have any way
	 * to say "create the object if it doesn't exist", and since the create
	 * flag destroys the underlying store, we can't just always set it.  If
	 * we fail with "invalid volume", try again with a create flag.
	 */
#if defined(KVS_O_TRUNCATE)
	This is fragile: once KVS_O_CREATE changes to not always destroy the
	store, we can set it all the time, and whatever new flag destroys the
	store should get pushed out into the api as a new kvs flag.  (See the
	above setting of KVS_O_CREATE for the kvs_open_o_truncate option.)
#endif
	ks->kvs = kvs_open(device_list, &kvs_config, flags);
	if (ks->kvs == NULL)
		ret = os_errno();
	if (ret == KVS_E_VOL_INVALID) {
		ret = 0;
		flags |= KVS_O_CREATE;
		ks->kvs = kvs_open(device_list, &kvs_config, flags);
		if (ks->kvs == NULL)
			ret = os_errno();
	}
	if (ks->kvs == NULL) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_open: %s: %s", ks->name, kvs_strerror(os_errno()));
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

	free(ks);
	if (device_list != NULL) {
		for (p = device_list; *p != NULL; ++p)
			free(*p);
		free(device_list);
	}
	free(devices);
	return (ret);
}

/*
 * uri_source_open --
 *	Return a locked URI, allocating and opening if it doesn't already exist.
 */
static int
uri_source_open(WT_DATA_SOURCE *wtds, WT_SESSION *session,
    WT_CONFIG_ARG *config, const char *uri, int hold_global, URI_SOURCE **refp)
{
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	int lockinit, ret = 0;

	*refp = NULL;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	lockinit = 0;

	/* Get the underlying KVS source. */
	if ((ret = kvs_source_open(wtds, session, config, &ks)) != 0)
		return (ret);

	/* Check for a match: if we find one, we're done. */
	for (us = ks->uri_head; us != NULL; us = us->next)
		if (strcmp(us->name, uri) == 0)
			goto done;

	/* Allocate and initialize a new underlying URI source object. */
	if ((us = calloc(1, sizeof(*us))) == NULL ||
	    (us->name = strdup(uri)) == NULL) {
		ret = os_errno();
		goto err;
	}
	if ((ret = lock_init(wtext, session, &us->lock)) != 0)
		goto err;
	lockinit = 1;
	us->ks = ks;

	/* Insert the new entry at the head of the list. */
	us->next = ks->uri_head;
	ks->uri_head = us;

	/* Return the locked object. */
done:	if ((ret = writelock(wtext, session, &us->lock)) != 0)
		goto err;

	*refp = us;
	us = NULL;

	if (0) {
err:		if (lockinit)
			ETRET(lock_destroy(wtext, session, &us->lock));
		if (us != NULL) {
			free(us->name);
			free(us);
		}
	}

	/* If our caller doesn't need it, release the global lock. */
	if (ret != 0 || !hold_global)
		ETRET(unlock(wtext, session, &ds->global_lock));
	return (ret);
}

/*
 * uri_truncate --
 *	Discard the records for an object.
 */
static int
uri_truncate(WT_DATA_SOURCE *wtds, WT_SESSION *session, URI_SOURCE *us)
{
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	kvs_t kvs;
	size_t i;
	int ret = 0;
	uint8_t *p, *t;
	char key[KVS_MAX_KEY_LEN];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;
	kvs = us->ks->kvs;

	/* Walk the list of objects, discarding them all. */
	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	memcpy(r->key, us->id, us->idlen);
	r->key_len = us->idlen;
	r->val = NULL;
	r->val_len = 0;
	while ((ret = kvs_next(kvs, r, 0UL, 0UL)) == 0) {
		/*
		 * Check for an object ID mismatch, if we find one, we're
		 * done.
		 */
		for (p = us->id, t = r->key, i = 0; i < us->idlen; ++i)
			if (*t++ != *p++)
				return (0);
		if ((ret = kvs_del(kvs, r)) != 0) {
			ESET(wtext, session,
			    WT_ERROR, "kvs_del: %s", kvs_strerror(ret));
			return (ret);
		}
	}
	if (ret == KVS_E_KEY_NOT_FOUND)
		ret = 0;
	if (ret != 0)
		ESET(wtext, session,
		    WT_ERROR, "kvs_next: %s", kvs_strerror(ret));
	return (ret);
}

/*
 * master_key_set --
 *	Set a master key from a URI, checking the length.
 */
static int
master_key_set(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *key, struct kvs_record *r)
{
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	size_t idlen, len;
	int ret = 0;
	char id[KVS_MAX_PACKED_8B];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	if ((ret = wtext->struct_size(wtext,
	    session, &idlen, "r", KVS_MASTER_ID)) != 0 ||
	    (ret = wtext->struct_pack(wtext,
	    session, id, sizeof(id), "r", KVS_MAX_PACKED_8B)) != 0)
		return (ret);

	/* Check to see if the ID and key can fit into a key. */
	len = strlen(key);
	if (idlen + len > KVS_MAX_KEY_LEN)
		ERET(wtext, session, EINVAL, "%s: too large for a Memrata key");

	memcpy((uint8_t *)r->key, id, idlen);
	memcpy((uint8_t *)r->key + idlen, key, len);
	r->key_len = idlen + len;
	return (0);
}

/*
 * master_id_get --
 *	Return the maximum file ID in the system.
 */
static int
master_id_get(
    WT_DATA_SOURCE *wtds, WT_SESSION *session, kvs_t kvs, uint64_t *maxidp)
{
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	char key[KVS_MAX_KEY_LEN], val[KVS_MASTER_VALUE_MAX];

	*maxidp = KVS_MAXID_BASE;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	if ((ret = master_key_set(wtds, session, KVS_MAXID, r)) != 0)
		return (ret);
	r->val = val;
	r->val_len = sizeof(val);
	switch (ret = kvs_get(kvs, r, 0UL, (unsigned long)sizeof(val))) {
	case 0:
		*maxidp = strtouq(r->val, NULL, 10);
		break;
	case KVS_E_KEY_NOT_FOUND:
		ret = 0;
		break;
	default:
		ERET(wtext, session,
		    WT_ERROR, "kvs_get: %s: %s", KVS_MAXID, kvs_strerror(ret));
		break;
	}
	return (ret);
}

/*
 * master_id_set --
 *	Increment the maximum file ID in the system.
 */
static int
master_id_set(
    WT_DATA_SOURCE *wtds, WT_SESSION *session, kvs_t kvs, uint64_t maxid)
{
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	char key[KVS_MAX_KEY_LEN], val[KVS_MASTER_VALUE_MAX];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	if ((ret = master_key_set(wtds, session, KVS_MAXID, r)) != 0)
		return (ret);
	r->val = val;
	r->val_len = snprintf(val, sizeof(val), "%" PRIu64, maxid);
	if ((ret = kvs_set(kvs, r)) != 0)
		ERET(wtext, session,
		    WT_ERROR, "kvs_set: %s: %s", KVS_MAXID, kvs_strerror(ret));
	if ((ret = kvs_commit(kvs)) != 0)
		ERET(wtext, session,
		    WT_ERROR, "kvs_commit: %s", kvs_strerror(ret));
	return (0);
}

/*
 * master_uri_get --
 *	Get the KVS master record for a URI.
 */
static int
master_uri_get(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, kvs_t kvs, char *val)
{
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	WT_EXTENSION_API *wtext;
	int ret = 0;
	char key[KVS_MAX_KEY_LEN];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	if ((ret = master_key_set(wtds, session, uri, r)) != 0)
		return (ret);
	r->val = val;
	r->val_len = KVS_MASTER_VALUE_MAX;
	if ((ret = kvs_get(kvs, r, 0UL, KVS_MASTER_VALUE_MAX)) == 0)
		return (0);
	if (ret == KVS_E_KEY_NOT_FOUND)
		return (WT_NOTFOUND);
	ERET(wtext, session, WT_ERROR, "kvs_get: %s", uri, kvs_strerror(ret));
}

/*
 * master_uri_set --
 *	Set the KVS master record for a URI.
 */
static int
master_uri_set(WT_DATA_SOURCE *wtds,
    WT_SESSION *session, const char *uri, kvs_t kvs, WT_CONFIG_ARG *config)
{
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	WT_CONFIG_ITEM a, b;
	WT_EXTENSION_API *wtext;
	uint64_t maxid;
	int ret = 0;
	char key[KVS_MAX_KEY_LEN], val[KVS_MASTER_VALUE_MAX];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Get the maximum file ID. */
	if ((ret = master_id_get(wtds, session, kvs, &maxid)) != 0)
		return (ret);
	++maxid;

	/* Get the key/value format strings. */
	if ((ret = wtext->config_get(
	    wtext, session, config, "key_format", &a)) != 0)
		ERET(wtext, session, ret,
		    "key_format configuration: %s", wtext->strerror(ret));
	if ((ret = wtext->config_get(
	    wtext, session, config, "value_format", &b)) != 0)
		ERET(wtext, session, ret,
		    "value_format configuration: %s", wtext->strerror(ret));

	/*
	 * Create a new reference using kvs_add (which fails if the record
	 * already exists).
	 */
	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	if ((ret = master_key_set(wtds, session, uri, r)) != 0)
		return (ret);
	r->val = val;
	r->val_len = snprintf(val, sizeof(val),
	    "key_generator=1,uid=%" PRIu64
	    ",version=(major=%d,minor=%d),key_format=%.*s,value_format=%.*s",
	    maxid, KVS_MAJOR, KVS_MINOR, a.len, a.str, b.len, b.str);
	if (r->val_len >= sizeof(val))
		ERET(wtext, session, WT_ERROR, "master URI value too large");
	++r->val_len;				/* Include the trailing nul. */
	switch (ret = kvs_add(kvs, r)) {
	case 0:
		if ((ret = master_id_set(wtds, session, kvs, maxid)) != 0)
			return (ret);
		if ((ret = kvs_commit(kvs)) != 0)
			ERET(wtext, session,
			    WT_ERROR, "kvs_commit: %s", kvs_strerror(ret));
		break;
	case KVS_E_KEY_EXISTS:
		break;
	default:
		ERET(wtext,
		    session, WT_ERROR, "kvs_add: %s", uri, kvs_strerror(ret));
	}
	return (0);
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
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	kvs_t kvs;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Get a locked reference to the URI. */
	if ((ret = uri_source_open(wtds, session, config, uri, 0, &us)) != 0)
		return (ret);
	kvs = us->ks->kvs;

	/* Create the URI master record if it doesn't already exist. */
	ret = master_uri_set(wtds, session, uri, kvs, config);

	/* Unlock the URI. */
err:	ETRET(unlock(wtext, session, &us->lock));

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
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	KVS_SOURCE *ks;
	URI_SOURCE **p, *us;
	WT_EXTENSION_API *wtext;
	kvs_t kvs;
	int ret = 0;
	char key[KVS_MAX_KEY_LEN], val[KVS_MASTER_VALUE_MAX];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Get a locked reference to the data source. */
	if ((ret = uri_source_open(wtds, session, config, uri, 1, &us)) != 0)
		return (ret);
	ks = us->ks;
	kvs = us->ks->kvs;

	/* If there are active references to the object, we're busy. */
	if (us->ref != 0) {
		ret = EBUSY;
		goto err;
	}

	/*
	 * Remove the entry from the URI_SOURCE list -- it's a singly-linked
	 * list, find the reference to it.
	 */
	for (p = &ks->uri_head; *p != NULL; p = &(*p)->next)
		if (*p == us)
			break;
	/*
	 * We should be guaranteed to find an entry, after all, we just looked
	 * it up, and everything is locked down.
	 */
	if (*p == NULL) {
		ret = WT_NOTFOUND;
		goto err;
	}
	*p = (*p)->next;

	/*
	 * Create a new reference to the newname, using kvs_add (which fails
	 * if the record already exists).
	 */
	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	if ((ret = master_key_set(wtds, session, uri, r)) != 0)
		goto err;
	r->val = NULL;
	r->val_len = 0;
	if ((ret = kvs_del(kvs, r)) != 0) {
		ESET(wtext, session, WT_ERROR,
		    "kvs_del: %s: %s", uri, kvs_strerror(ret));
		goto err;
	}

	ret = uri_truncate(wtds, session, us);

	kvs_dump(wtext, session, ks->kvs, "after drop");

err:	ETRET(unlock(wtext, session, &us->lock));
	ETRET(unlock(wtext, session, &ds->global_lock));
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
	URI_SOURCE *us;
	WT_CONFIG_ITEM v;
	WT_CURSOR *wtcursor;
	WT_EXTENSION_API *wtext;
	kvs_t kvs;
	size_t size;
	int ret = 0;
	const char *cfg[2];
	char val[KVS_MASTER_VALUE_MAX];

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

	/* Get a locked reference to the URI. */
	if ((ret = uri_source_open(wtds, session, config, uri, 0, &us)) != 0)
		goto err;
	kvs = us->ks->kvs;

	/*
	 * Finish initializing the cursor (if the URI_SOURCE structure requires
	 * initialization, we're going to use the cursor as part of that work).
	 */
	cursor->ks = us->ks;
	cursor->us = us;

	/*
	 * If this is the first access to the URI, we have to configure it
	 * using information stored in the master record.
	 */
	if (!us->configured) {
		us->configured = 1;

		master_uri_get(wtds, session, uri, kvs, val);
		cfg[0] = val;
		cfg[1] = NULL;
		if ((ret = wtext->config_get(wtext,
		    session, (WT_CONFIG_ARG *)cfg, "uid", &v)) != 0) {
			ESET(wtext, session, ret,
			    "WT_EXTENSION_API.config: uid: %s",
			    wtext->strerror(ret));
			goto err;
		}

		/*
		 * Build packed versions of the unique ID and the next ID (the
		 * next ID is what we need to do a "previous" cursor traversal.)
		 */
		if ((ret = wtext->struct_size(wtext,
		    session, &us->idlen, "r", v.val)) != 0 ||
		    (ret = wtext->struct_pack(wtext,
		    session, us->id, sizeof(us->id), "r", v.val)) != 0 ||
		    (ret = wtext->struct_size(wtext,
		    session, &us->idnextlen, "r", v.val + 1)) != 0 ||
		    (ret = wtext->struct_pack(wtext, session,
		    us->idnext, sizeof(us->idnext), "r", v.val + 1)) != 0) {
			ESET(wtext, session, ret,
			    "WT_EXTENSION_API.config: uid: %s",
			    wtext->strerror(ret));
			goto err;
		}

		if ((ret = wtext->config_get(wtext,
		    session, (WT_CONFIG_ARG *)cfg, "key_format", &v)) != 0) {
			ESET(wtext, session, ret,
			    "key_format configuration: %s",
			    wtext->strerror(ret));
			goto err;
		}
		us->config_recno = v.len == 1 && v.str[0] == 'r';

		if ((ret = wtext->config_get(wtext,
		    session, (WT_CONFIG_ARG *)cfg, "value_format", &v)) != 0) {
			ESET(wtext, session, ret,
			    "value_format configuration: %s",
			    wtext->strerror(ret));
			goto err;
		}
		us->config_bitfield =
		    v.len == 2 && isdigit(v.str[0]) && v.str[1] == 't';

		/*
		 * If it's a record-number key, read the last record from the
		 * object and set the allocation record value.
		 */
		if (us->config_recno) {
			wtcursor = (WT_CURSOR *)cursor;
			if ((ret = kvs_cursor_reset(wtcursor)) != 0)
				goto err;

			if ((ret = kvs_cursor_prev(wtcursor)) == 0)
				us->append_recno = wtcursor->recno;
			else if (ret != WT_NOTFOUND)
				goto err;

			if ((ret = kvs_cursor_reset(wtcursor)) != 0)
				goto err;
		}
	}

	/* Increment the open reference count to pin the URI and unlock it. */
	++us->ref;
	if ((ret = unlock(wtext, session, &us->lock)) != 0)
		goto err;

	*new_cursor = (WT_CURSOR *)cursor;

	if (0) {
err:		if (cursor != NULL) {
			free(cursor->val);
			free(cursor);
		}
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
	struct kvs_record *r, _r;
	DATA_SOURCE *ds;
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	kvs_t kvs;
	int ret = 0;
	char *copy, key[KVS_MAX_KEY_LEN], val[KVS_MASTER_VALUE_MAX];

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Get a copy of the new name. */
	if ((copy = strdup(newname)) == NULL)
		return (os_errno());

	/* Get a locked reference to the URI. */
	if ((ret = uri_source_open(wtds, session, config, uri, 1, &us)) != 0)
		return (ret);
	kvs = us->ks->kvs;

	/* If there are active references to the object, we're busy. */
	if (us->ref != 0) {
		ret = EBUSY;
		goto err;
	}

	/* Get the master record for the original name. */
	if ((ret = master_uri_get(wtds, session, uri, kvs, val)) != 0)
		goto err;

	/*
	 * Create a new reference to the newname, using kvs_add (which fails
	 * if the record already exists).
	 */
	r = &_r;
	memset(r, 0, sizeof(*r));
	r->key = key;
	if ((ret = master_key_set(wtds, session, newname, r)) != 0)
		goto err;
	r->val = val;
	r->val_len = strlen(val) + 1;		/* Include the trailing nul. */
	switch (ret = kvs_add(kvs, r)) {
	case 0:
		/* Remove the original entry. */
		if ((ret = master_key_set(wtds, session, uri, r)) != 0)
			goto cleanup;
		r->val = NULL;
		r->val_len = 0;
		if ((ret = kvs_del(kvs, r)) != 0) {
			ESET(wtext, session, WT_ERROR,
			    "kvs_del: %s: %s", uri, kvs_strerror(ret));

cleanup:		/*
			 * Try and remove the new entry if we can't remove the
			 * old entry.
			 */
			if ((ret =
			    master_key_set(wtds, session, newname, r)) == 0 &&
			    (ret = kvs_del(kvs, r)) != 0)
				ESET(wtext, session, WT_ERROR,
				    "kvs_del: %s: %s",
				    newname, kvs_strerror(ret));
			goto err;
		}

		/* Flush the change. */
		if ((ret = kvs_commit(kvs)) != 0) {
			ESET(wtext, session,
			    WT_ERROR, "kvs_commit: %s", kvs_strerror(ret));
			goto err;
		}
		break;
	case KVS_E_KEY_EXISTS:
		ESET(wtext, session, EEXIST, "%s", newname);
		break;
	default:
		ESET(wtext, session,
		    WT_ERROR, "kvs_add: %s", newname, kvs_strerror(ret));
		break;
	}

err:	ETRET(unlock(wtext, session, &us->lock));
	ETRET(unlock(wtext, session, &ds->global_lock));
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
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	int ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	/* Get a locked reference to the URI. */
	if ((ret = uri_source_open(wtds, session, config, uri, 0, &us)) != 0)
		return (ret);

	/*
	 * If there are active references to the object, we're busy.
	 * Else, discard the records.
	 */
	ret = us->ref == 0 ? uri_truncate(wtds, session, us) : EBUSY;

	ETRET(unlock(wtext, session, &us->lock));
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
	URI_SOURCE *us;
	WT_EXTENSION_API *wtext;
	int tret, ret = 0;

	ds = (DATA_SOURCE *)wtds;
	wtext = ds->wtext;

	ret = writelock(wtext, session, &ds->global_lock);

	/* Start a flush on any open KVS sources. */
	for (ks = ds->kvs_head; ks != NULL; ks = ks->next)
		if ((tret = kvs_commit(ks->kvs)) != 0)
			ESET(wtext, session, WT_ERROR,
			    "kvs_commit: %s: %s", ks->name, kvs_strerror(tret));

	/* Close and discard all objects. */
	while ((ks = ds->kvs_head) != NULL) {
		while ((us = ks->uri_head) != NULL) {
			if (us->ref != 0)
				ESET(wtext, session, WT_ERROR,
				    "%s: has open object %s with %u open "
				    "cursors during close",
				    ks->name, us->name, us->ref);

			ks->uri_head = us->next;
			ETRET(lock_destroy(wtext, session, &us->lock));
			free(us->name);
			free(us);
		}

		if ((tret = kvs_close(ks->kvs)) != 0)
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
