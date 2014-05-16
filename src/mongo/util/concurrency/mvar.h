// mvar.h

/*    Copyright 2009 10gen Inc.
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
