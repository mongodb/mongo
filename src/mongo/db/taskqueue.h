// @file deferredinvoker.h

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "mongomutex.h"

// if you turn this back on be sure to enable TaskQueueTest again
#if 0

namespace mongo {

    /** defer work items by queueing them for invocation by another thread.  presumption is that
        consumer thread is outside of locks more than the source thread.  Additional presumption
        is that several objects or micro-tasks will be queued and that having a single thread
        processing them in batch is hepful as they (in the first use case) use a common data
        structure that can then be in local cpu classes.

        this class is in db/ as it is dbMutex (mongomutex) specific (so far).

        using a functor instead of go() might be more elegant too, once again, would like to test any
        performance differential.  also worry that operator() hides things?

        MT - copyable "micro task" object we can queue
             must have a static method void MT::go(const MT&)

        see DefInvoke in dbtests/ for an example.
    */
    template< class MT >
    class TaskQueue {
    public:
        TaskQueue() : _which(0), _invokeMutex("deferredinvoker") { }

        void defer(MT mt) {
            // only one writer allowed.  however the invoke processing below can occur concurrently with
            // writes (for the most part)
            DEV verify( Lock::isW() );

            _queues[_which].push_back(mt);
        }

        /** call to process deferrals.

            concurrency: handled herein.  multiple threads could call invoke(), but their efforts will be
                         serialized.  the common case is that there is a single processor calling invoke().

            normally, you call this outside of any lock.  but if you want to fully drain the queue,
            call from within a read lock.  for example:
            {
              // drain with minimal time in lock
              d.invoke();
              readlock lk;
              d.invoke();
              ...
            }
            you can also call invoke periodically to do some work and then pick up later on more.
        */
        void invoke() {
            mutex::scoped_lock lk2(_invokeMutex);
            int toDrain = 0;
            {
                // flip queueing to the other queue (we are double buffered)
                readlocktry lk(5);
                if( !lk.got() )
                    return;
                toDrain = _which;
                _which = _which ^ 1;
                wassert( _queues[_which].empty() ); // we are in dbMutex, so it should be/stay empty til we exit dbMutex
            }

            _drain( _queues[toDrain] );
            verify( _queues[toDrain].empty() );
        }

    private:
        int _which; // 0 or 1
        typedef vector< MT > Queue;
        Queue _queues[2];

        // lock order when multiple locks: dbMutex, _invokeMutex
        mongo::mutex _invokeMutex;

        void _drain(Queue& queue) {
            unsigned oldCap = queue.capacity();
            for( typename Queue::iterator i = queue.begin(); i != queue.end(); i++ ) {
                const MT& v = *i;
                MT::go(v);
            }
            queue.clear();
            DEV verify( queue.capacity() == oldCap ); // just checking that clear() doesn't deallocate, we don't want that
        }
    };

}

#endif
