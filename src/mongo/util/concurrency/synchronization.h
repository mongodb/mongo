// synchronization.h

/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <boost/thread/condition.hpp>
#include "mutex.h"

namespace mongo {

    /*
     * A class to establish a synchronization point between two threads. One thread is the waiter and one is
     * the notifier. After the notification event, both proceed normally.
     *
     * This class is thread-safe.
     */
    class Notification : boost::noncopyable {
    public:
        Notification();

        /*
         * Blocks until the method 'notifyOne()' is called.
         */
        void waitToBeNotified();

        /*
         * Notifies the waiter of '*this' that it can proceed.  Can only be called once.
         */
        void notifyOne();

    private:
        mongo::mutex _mutex;          // protects state below
        unsigned long long lookFor;
        unsigned long long cur;
        boost::condition _condition;  // cond over _notified being true
    };

    /** establishes a synchronization point between threads. N threads are waits and one is notifier.
        threadsafe.
    */
    class NotifyAll : boost::noncopyable {
    public:
        NotifyAll();

        typedef unsigned long long When;

        When now();

        /** awaits the next notifyAll() call by another thread. notifications that precede this
            call are ignored -- we are looking for a fresh event.
        */
        void waitFor(When);

        /** a bit faster than waitFor( now() ) */
        void awaitBeyondNow();

        /** may be called multiple times. notifies all waiters */
        void notifyAll(When);

        /** indicates how many threads are waiting for a notify. */
        unsigned nWaiting() const { return _nWaiting; }

    private:
        mongo::mutex _mutex;
        boost::condition _condition;
        When _lastDone;
        When _lastReturned;
        unsigned _nWaiting;
    };

} // namespace mongo
