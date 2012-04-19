// atomic_uint64.h

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

#include <boost/thread/mutex.hpp>

namespace mongo {

    /**
     * atomic 64 bit unsingned integer
     * note: this implementation is meant to be tossed
     *       its super slow, but was quick to write for a prototype
     */
    class AtomicUInt64 {
    public:
        AtomicUInt64() {
            _counter = 0;
        }
        
        operator unsigned long long() const { return get(); }
        unsigned long long get() const;

        inline void set(unsigned long long newValue);
        inline void add(unsigned long long by );
        AtomicUInt64& operator+=( unsigned long long by ){ add( by ); return *this; }
        
        inline void zero() { set(0); }


    private:
        mutable boost::mutex _mutex;
        volatile unsigned long long _counter;
    };

    
    inline unsigned long long AtomicUInt64::get() const { 
        boost::mutex::scoped_lock lk( _mutex );
        return _counter;
    }

    inline void AtomicUInt64::set(unsigned long long newValue) {
        boost::mutex::scoped_lock lk( _mutex );
        _counter = newValue;
    }        

    inline void AtomicUInt64::add(unsigned long long by) {
        boost::mutex::scoped_lock lk( _mutex );
        _counter += by;
    }
    
}

