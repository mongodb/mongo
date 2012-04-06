// mvar.h

/*    Copyright 2009 10gen Inc.
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

#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/condition.hpp>

namespace mongo {

    /* This is based on haskell's MVar synchronization primitive:
     * http://www.haskell.org/ghc/docs/latest/html/libraries/base/Control-Concurrent-MVar.html
     *
     * It is a thread-safe queue that can hold at most one object.
     * You can also think of it as a box that can be either full or empty.
     */

    template <typename T>
    class MVar {
    public:
        enum State {EMPTY=0, FULL};

        // create an empty MVar
        MVar()
            : _state(EMPTY)
        {}

        // creates a full MVar
        MVar(const T& val)
            : _state(FULL)
            , _value(val)
        {}

        // puts val into the MVar and returns true or returns false if full
        // never blocks
        bool tryPut(const T& val) {
            // intentionally repeat test before and after lock
            if (_state == FULL) return false;
            Mutex::scoped_lock lock(_mutex);
            if (_state == FULL) return false;

            _state = FULL;
            _value = val;

            // unblock threads waiting to 'take'
            _condition.notify_all();

            return true;
        }

        // puts val into the MVar
        // will block if the MVar is already full
        void put(const T& val) {
            Mutex::scoped_lock lock(_mutex);
            while (!tryPut(val)) {
                // unlocks lock while waiting and relocks before returning
                _condition.wait(lock);
            }
        }

        // takes val out of the MVar and returns true or returns false if empty
        // never blocks
        bool tryTake(T& out) {
            // intentionally repeat test before and after lock
            if (_state == EMPTY) return false;
            Mutex::scoped_lock lock(_mutex);
            if (_state == EMPTY) return false;

            _state = EMPTY;
            out = _value;

            // unblock threads waiting to 'put'
            _condition.notify_all();

            return true;
        }

        // takes val out of the MVar
        // will block if the MVar is empty
        T take() {
            T ret = T();

            Mutex::scoped_lock lock(_mutex);
            while (!tryTake(ret)) {
                // unlocks lock while waiting and relocks before returning
                _condition.wait(lock);
            }

            return ret;
        }


        // Note: this is fast because there is no locking, but state could
        // change before you get a chance to act on it.
        // Mainly useful for sanity checks / asserts.
        State getState() { return _state; }


    private:
        State _state;
        T _value;
        typedef boost::recursive_mutex Mutex;
        Mutex _mutex;
        boost::condition _condition;
    };

}
