/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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

/*
 * Based on "Spinlocks and Read-Write Locks" by Dr. Steven Fuerst:
 *	http://locklessinc.com/articles/locks/
 *
 * Dr. Fuerst further credits:
 *	There exists a form of the ticket lock that is designed for read-write
 * locks. An example written in assembly was posted to the Linux kernel mailing
 * list in 2002 by David Howells from RedHat. This was a highly optimized
 * version of a read-write ticket lock developed at IBM in the early 90's by
 * Joseph Seigh. Note that a similar (but not identical) algorithm was published
 * by John Mellor-Crummey and Michael Scott in their landmark paper "Scalable
 * Reader-Writer Synchronization for Shared-Memory Multiprocessors".
 */

#include "wt_internal.h"

/*
 * __wt_rwlock_alloc --
 *	Allocate and initialize a read/write lock.
 */
int
__wt_rwlock_alloc(
    WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp, const char *name)
{
	WT_RWLOCK *rwlock;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: alloc %s", name));

	WT_RET(__wt_calloc_one(session, &rwlock));

	rwlock->name = name;

	*rwlockp = rwlock;
	return (0);
}

/*
 * __wt_try_readlock --
 *	Try to get a shared lock, fail immediately if unavailable.
 */
int
__wt_try_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t old, new, pad, users, writers;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: try_readlock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	l = &rwlock->rwlock;
	pad = l->s.pad;
	users = l->s.users;
	writers = l->s.writers;
	old = (pad << 48) + (users << 32) + (users << 16) + writers;
	new = (pad << 48) + ((users + 1) << 32) + ((users + 1) << 16) + writers;
	return (WT_ATOMIC_CAS8(l->u, old, new) ? 0 : EBUSY);
}

/*
 * __wt_readlock --
 *	Get a shared lock.
 */
int
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t me;
	uint16_t val;
	int pause_cnt;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: readlock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	l = &rwlock->rwlock;
	me = WT_ATOMIC_FETCH_ADD8(l->u, (uint64_t)1 << 32);
	val = (uint16_t)(me >> 32);
	for (pause_cnt = 0; val != l->s.readers;) {
		/*
		 * We failed to get the lock; pause before retrying and if we've
		 * paused enough, sleep so we don't burn CPU to no purpose. This
		 * situation happens if there are more threads than cores in the
		 * system and we're thrashing on shared resources. Regardless,
		 * don't sleep long, all we need is to schedule the other reader
		 * threads to complete a few more instructions and increment the
		 * reader count.
		 */
		if (++pause_cnt < 1000)
			WT_PAUSE();
		else
			__wt_sleep(0, 10);
	}

	++l->s.readers;

	return (0);
}

/*
 * __wt_readunlock --
 *	Release a shared lock.
 */
int
__wt_readunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: read unlock %s", rwlock->name));

	l = &rwlock->rwlock;
	WT_ATOMIC_ADD2(l->s.writers, 1);

	return (0);
}

/*
 * __wt_try_writelock --
 *	Try to get an exclusive lock, fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t old, new, pad, readers, users;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: try_writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	l = &rwlock->rwlock;
	pad = l->s.pad;
	readers = l->s.readers;
	users = l->s.users;
	old = (pad << 48) + (users << 32) + (readers << 16) + users;
	new = (pad << 48) + ((users + 1) << 32) + (readers << 16) + users;
	return (WT_ATOMIC_CAS8(l->u, old, new) ? 0 : EBUSY);
}

/*
 * __wt_writelock --
 *	Wait to get an exclusive lock.
 */
int
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t me;
	uint16_t val;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	/*
	 * Possibly wrap: if we have more than 64K lockers waiting, the count
	 * of writers will wrap and two lockers will simultaneously be granted
	 * the write lock.
	 */
	l = &rwlock->rwlock;
	me = WT_ATOMIC_FETCH_ADD8(l->u, (uint64_t)1 << 32);
	val = (uint16_t)(me >> 32);
	while (val != l->s.writers)
		WT_PAUSE();

	return (0);
}

/*
 * __wt_writeunlock --
 *	Release an exclusive lock.
 */
int
__wt_writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l, copy;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: writeunlock %s", rwlock->name));

	l = &rwlock->rwlock;

	copy = *l;

	WT_BARRIER();

	++copy.s.writers;
	++copy.s.readers;

	l->us = copy.us;
	return (0);
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a read/write lock.
 */
int
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp)
{
	WT_RWLOCK *rwlock;

	rwlock = *rwlockp;		/* Clear our caller's reference. */
	if (rwlock == NULL)
		return (0);
	*rwlockp = NULL;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: destroy %s", rwlock->name));

	__wt_free(session, rwlock);
	return (0);
}
