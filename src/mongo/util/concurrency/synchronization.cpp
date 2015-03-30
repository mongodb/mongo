// synchronization.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "synchronization.h"

#include <boost/date_time/posix_time/posix_time.hpp>

#include "mongo/util/log.h"

namespace mongo {

namespace {
    ThreadIdleCallback threadIdleCallback;
} // namespace

    void registerThreadIdleCallback(ThreadIdleCallback callback) {
        invariant(!threadIdleCallback);
        threadIdleCallback = callback;
    }

    void markThreadIdle() {
        if (!threadIdleCallback) {
            return;
        }
        try {
            threadIdleCallback();
        }
        catch (...) {
            severe() << "Exception escaped from threadIdleCallback";
            fassertFailedNoTrace(28603);
        }
    }

    Notification::Notification() {
        lookFor = 1;
        cur = 0;
    }

    void Notification::waitToBeNotified() {
        boost::unique_lock<boost::mutex> lock( _mutex );
        while ( lookFor != cur )
            _condition.wait(lock);
        lookFor++;
    }

    void Notification::notifyOne() {
        boost::lock_guard<boost::mutex> lock( _mutex );
        verify( cur != lookFor );
        cur++;
        _condition.notify_one();
    }

    /* --- NotifyAll --- */

    NotifyAll::NotifyAll() {
        _lastDone = 0;
        _lastReturned = 0;
        _nWaiting = 0;
    }

    NotifyAll::When NotifyAll::now() { 
        boost::lock_guard<boost::mutex> lock( _mutex );
        return ++_lastReturned;
    }

    void NotifyAll::waitFor(When e) {
        boost::unique_lock<boost::mutex> lock( _mutex );
        ++_nWaiting;
        while( _lastDone < e ) {
            _condition.wait(lock);
        }
    }

    void NotifyAll::awaitBeyondNow() { 
        boost::unique_lock<boost::mutex> lock( _mutex );
        ++_nWaiting;
        When e = ++_lastReturned;
        while( _lastDone <= e ) {
            _condition.wait(lock);
        }
    }

    void NotifyAll::notifyAll(When e) {
        boost::unique_lock<boost::mutex> lock( _mutex );
        _lastDone = e;
        _nWaiting = 0;
        _condition.notify_all();
    }

} // namespace mongo
