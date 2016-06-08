/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * Establishes a synchronization point between threads. N threads are waits and one is notifier.
 */
class CommitNotifier {
    MONGO_DISALLOW_COPYING(CommitNotifier);

public:
    typedef unsigned long long When;

    CommitNotifier();
    ~CommitNotifier();

    When now();

    /**
     * Awaits the next notifyAll() call by another thread. notifications that precede this call are
     * ignored -- we are looking for a fresh event.
     */
    void waitFor(When e);

    /**
     * A bit faster than waitFor(now()).
     */
    void awaitBeyondNow();

    /**
     * May be called multiple times. Notifies all waiters.
     */
    void notifyAll(When e);

    /**
     * Returns how many threads are blocked in the waitFor/awaitBeyondNow calls.
     */
    unsigned nWaiting() const {
        return _nWaiting;
    }

private:
    stdx::mutex _mutex;
    stdx::condition_variable _condition;

    When _lastDone{0};
    When _lastReturned{0};
    unsigned _nWaiting{0};
};

}  // namespace mongo
