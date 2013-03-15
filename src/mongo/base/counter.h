// counter.h

/**
*    Copyright (C) 2008-2012 10gen Inc.
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
*/

#pragma once

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/cstdint.h"

namespace mongo {
    /**
     * A 64bit (atomic) counter.
     *
     * The constructor allows setting the start value, and increment([int]) is used to change it.
     *
     * The value can be returned using get() or the (long long) function operator.
     */
    class Counter64 {
    public:

        /** Atomically increment. */
        void increment( uint64_t n = 1 ) { _counter.addAndFetch(n); }

        /** Atomically decrement. */
        void decrement( uint64_t n = 1 ) { _counter.subtractAndFetch(n); }

        /** Return the current value */
        long long get() const { return _counter.load(); }
        
        operator long long() const { return get(); }
    private:
        AtomicInt64 _counter;
    };
}
