// synchronization.h

/*    Copyright 2010 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/thread/condition.hpp>
#include <boost/noncopyable.hpp>

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
