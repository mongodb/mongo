#pragma once

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/platform/unordered_map.h"

namespace mongo {

    /** Thread safe map.
        Be careful not to use this too much or it could make things slow;
        if not a hot code path no problem.
        Examples:

        mapsf< map<int,int>, int, int > mp;

        int x = mp.get();

        map< map<int,int>, int, int > two;
        mp.swap(two);

        {
            mapsf< map<int,int>, int, int >::ref r(mp);
            r[9] = 1;
            map<int,int>::iterator i = r.r.begin();
        }
    */
    template< class M >
    struct mapsf : boost::noncopyable {
        SimpleMutex m;
        M val;
        friend struct ref;
    public:

        typedef typename M::const_iterator const_iterator;
        typedef typename M::key_type key_type;
        typedef typename M::mapped_type mapped_type;

        mapsf() : m("mapsf") { }
        void swap(M& rhs) {
            SimpleMutex::scoped_lock lk(m);
            val.swap(rhs);
        }
        bool empty() {
            SimpleMutex::scoped_lock lk(m);
            return val.empty();
        }
        // safe as we pass by value:
        mapped_type get(key_type k) {
            SimpleMutex::scoped_lock lk(m);
            const_iterator i = val.find(k);
            if( i == val.end() )
                return mapped_type();
            return i->second;
        }
        // think about deadlocks when using ref.  the other methods
        // above will always be safe as they are "leaf" operations.
        struct ref {
            SimpleMutex::scoped_lock lk;
        public:
            M &r;
            ref(mapsf &m) : lk(m.m), r(m.val) { }
            mapped_type& operator[](const key_type& k) { return r[k]; }
        };
    };

}
