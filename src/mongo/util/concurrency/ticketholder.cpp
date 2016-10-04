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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/ticketholder.h"

#include <iostream>

#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

#if defined(__linux__)
namespace {
void _check(int ret) {
    if (ret == 0)
        return;
    int err = errno;
    severe() << "error in Ticketholder: " << errnoWithDescription(err);
    fassertFailed(28604);
}
}

TicketHolder::TicketHolder(int num) : _outof(num) {
    _check(sem_init(&_sem, 0, num));
}

TicketHolder::~TicketHolder() {
    _check(sem_destroy(&_sem));
}

bool TicketHolder::tryAcquire() {
    while (0 != sem_trywait(&_sem)) {
        switch (errno) {
            case EAGAIN:
                return false;
            case EINTR:
                break;
            default:
                _check(-1);
        }
    }
    return true;
}

void TicketHolder::waitForTicket() {
    while (0 != sem_wait(&_sem)) {
        switch (errno) {
            case EINTR:
                break;
            default:
                _check(-1);
        }
    }
}

void TicketHolder::release() {
    _check(sem_post(&_sem));
}

Status TicketHolder::resize(int newSize) {
    stdx::lock_guard<stdx::mutex> lk(_resizeMutex);

    if (newSize < 5)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Minimum value for semaphore is 5; given " << newSize);

    if (newSize > SEM_VALUE_MAX)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Maximum value for semaphore is " << SEM_VALUE_MAX
                                    << "; given "
                                    << newSize);

    while (_outof.load() < newSize) {
        release();
        _outof.fetchAndAdd(1);
    }

    while (_outof.load() > newSize) {
        waitForTicket();
        _outof.subtractAndFetch(1);
    }

    invariant(_outof.load() == newSize);
    return Status::OK();
}

int TicketHolder::available() const {
    int val = 0;
    _check(sem_getvalue(&_sem, &val));
    return val;
}

int TicketHolder::used() const {
    return outof() - available();
}

int TicketHolder::outof() const {
    return _outof.load();
}

#else

TicketHolder::TicketHolder(int num) : _outof(num), _num(num) {}

TicketHolder::~TicketHolder() = default;

bool TicketHolder::tryAcquire() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _tryAcquire();
}

void TicketHolder::waitForTicket() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    while (!_tryAcquire()) {
        _newTicket.wait(lk);
    }
}

void TicketHolder::release() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _num++;
    }
    _newTicket.notify_one();
}

Status TicketHolder::resize(int newSize) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    int used = _outof.load() - _num;
    if (used > newSize) {
        std::stringstream ss;
        ss << "can't resize since we're using (" << used << ") "
           << "more than newSize(" << newSize << ")";

        std::string errmsg = ss.str();
        log() << errmsg;
        return Status(ErrorCodes::BadValue, errmsg);
    }

    _outof.store(newSize);
    _num = _outof.load() - used;

    // Potentially wasteful, but easier to see is correct
    _newTicket.notify_all();
    return Status::OK();
}

int TicketHolder::available() const {
    return _num;
}

int TicketHolder::used() const {
    return outof() - _num;
}

int TicketHolder::outof() const {
    return _outof.load();
}

bool TicketHolder::_tryAcquire() {
    if (_num <= 0) {
        if (_num < 0) {
            std::cerr << "DISASTER! in TicketHolder" << std::endl;
        }
        return false;
    }
    _num--;
    return true;
}
#endif
}
