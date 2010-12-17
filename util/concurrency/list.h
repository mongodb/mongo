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

/* this class uses a mutex for writes, but not for reads. 
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
        T* next() const { return _next; }
    };

    T* head() const { return _head; }

    void push(T* t) {
        scoped_lock lk(_m);
        t->_next = _head;
        _head = t; 
    }

    // intentionally leak.
    void orphanAll() { 
        _head = 0;
    }

    /* t is not deleted, but is removed from the list. (orphaned) */
    void orphan(T* t) { 
        scoped_lock lk(_m);
        T *&prev = _head;
        T *n = prev;
        while( n != t ) {
            prev = n->_next;
            n = prev;
        }
        prev = t->_next;
        if( ++_orphans > 500 ) 
            log() << "warning orphans=" << _orphans << '\n';
    }

private:
    T *_head;
    mongo::mutex _m;
    int _orphans;
};

};
