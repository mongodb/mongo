// synchronization.cpp

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

#include "pch.h"
#include "synchronization.h"

namespace mongo {

    Notification::Notification() : _mutex ( "Notification" ){ 
        lookFor = 1;
        cur = 0;
    }

    void Notification::waitToBeNotified() {
        scoped_lock lock( _mutex );
        while ( lookFor != cur )
            _condition.wait( lock.boost() );
        lookFor++;
    }

    void Notification::notifyOne() {
        scoped_lock lock( _mutex );
        verify( cur != lookFor );
        cur++;
        _condition.notify_one();
    }

    /* --- NotifyAll --- */

    NotifyAll::NotifyAll() : _mutex("NotifyAll") { 
        _lastDone = 0;
        _lastReturned = 0;
        _nWaiting = 0;
    }

    NotifyAll::When NotifyAll::now() { 
        scoped_lock lock( _mutex );
        return ++_lastReturned;
    }

    void NotifyAll::waitFor(When e) {
        scoped_lock lock( _mutex );
        ++_nWaiting;
        while( _lastDone < e ) {
            _condition.wait( lock.boost() );
        }
    }

    void NotifyAll::awaitBeyondNow() { 
        scoped_lock lock( _mutex );
        ++_nWaiting;
        When e = ++_lastReturned;
        while( _lastDone <= e ) {
            _condition.wait( lock.boost() );
        }
    }

    void NotifyAll::notifyAll(When e) {
        scoped_lock lock( _mutex );
        _lastDone = e;
        _nWaiting = 0;
        _condition.notify_all();
    }

} // namespace mongo
