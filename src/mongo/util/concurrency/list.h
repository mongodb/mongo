// list.h

/**
*    Copyright (C) 2008 10gen Inc.
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

namespace mongo {

    /* DONT USE THIS.  it was a dumb idea.
    
       this class uses a mutex for writes, but not for reads.
       we can get fancier later...

            struct Member : public List1<Member>::Base {
                const char *host;
                int port;
            };
            List1<Member> _members;
            _members.head()->next();

    */
    template<typename T>
    class List1 : boost::noncopyable {
    public:
        /* next() and head() return 0 at end of list */

        List1() : _head(0), _m("List1"), _orphans(0) { }

        class Base {
            friend class List1;
            T *_next;
        public:
            Base() : _next(0){}
            ~Base() { wassert(false); } // we never want this to happen
            T* next() const { return _next; }
        };

        /** note this is safe: 

              T* p = mylist.head();
              if( p ) 
                use(p);

            and this is not:

              if( mylist.head() )
                use( mylist.head() ); // could become 0
        */
        T* head() const { return (T*) _head; }

        void push(T* t) {
            verify( t->_next == 0 );
            scoped_lock lk(_m);
            t->_next = (T*) _head;
            _head = t;
        }

        // intentionally leaks.
        void orphanAll() {
            scoped_lock lk(_m);
            _head = 0;
        }

        /* t is not deleted, but is removed from the list. (orphaned) */
        void orphan(T* t) {
            scoped_lock lk(_m);
            T *&prev = (T*&) _head;
            T *n = prev;
            while( n != t ) {
                uassert( 14050 , "List1: item to orphan not in list", n );
                prev = n->_next;
                n = prev;
            }
            prev = t->_next;
            if( ++_orphans > 500 )
                log() << "warning List1 orphans=" << _orphans << '\n';
        }

    private:
        volatile T *_head;
        mongo::mutex _m;
        int _orphans;
    };

};
