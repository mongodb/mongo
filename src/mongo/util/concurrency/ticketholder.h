/*    Copyright 2015 MongoDB Inc.
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

#if defined(__linux__)
#include <semaphore.h>
#endif

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

class TicketHolder {
    MONGO_DISALLOW_COPYING(TicketHolder);

public:
    explicit TicketHolder(int num);
    ~TicketHolder();

    bool tryAcquire();

    void waitForTicket();

    void release();

    Status resize(int newSize);

    int available() const;

    int used() const;

    int outof() const;

private:
#if defined(__linux__)
    mutable sem_t _sem;

    // You can read _outof without a lock, but have to hold _resizeMutex to change.
    AtomicInt32 _outof;
    stdx::mutex _resizeMutex;
#else
    bool _tryAcquire();

    AtomicInt32 _outof;
    int _num;
    stdx::mutex _mutex;
    stdx::condition_variable _newTicket;
#endif
};

class ScopedTicket {
public:
    ScopedTicket(TicketHolder* holder) : _holder(holder) {
        _holder->waitForTicket();
    }

    ~ScopedTicket() {
        _holder->release();
    }

private:
    TicketHolder* _holder;
};

class TicketHolderReleaser {
    MONGO_DISALLOW_COPYING(TicketHolderReleaser);

public:
    TicketHolderReleaser() {
        _holder = NULL;
    }

    explicit TicketHolderReleaser(TicketHolder* holder) {
        _holder = holder;
    }

    ~TicketHolderReleaser() {
        if (_holder) {
            _holder->release();
        }
    }

    bool hasTicket() const {
        return _holder != NULL;
    }

    void reset(TicketHolder* holder = NULL) {
        if (_holder) {
            _holder->release();
        }
        _holder = holder;
    }

private:
    TicketHolder* _holder;
};
}
