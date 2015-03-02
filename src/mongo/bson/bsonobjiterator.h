// bsonobjiterator.h

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

#include <boost/preprocessor/cat.hpp> // like the ## operator but works with __LINE__
#include <boost/scoped_array.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/base/disallow_copying.h"

namespace mongo {

    /** iterator for a BSONObj

       Note each BSONObj ends with an EOO element: so you will get more() on an empty
       object, although next().eoo() will be true.

       The BSONObj must stay in scope for the duration of the iterator's execution.

       todo: we may want to make a more stl-like iterator interface for this
             with things like begin() and end()
    */
    class BSONObjIterator {
    public:
        /** Create an iterator for a BSON object.
        */
        BSONObjIterator(const BSONObj& jso) {
            int sz = jso.objsize();
            if ( MONGO_unlikely(sz == 0) ) {
                _pos = _theend = 0;
                return;
            }
            _pos = jso.objdata() + 4;
            _theend = jso.objdata() + sz - 1;
        }

        BSONObjIterator( const char * start , const char * end ) {
            _pos = start + 4;
            _theend = end - 1;
        }

        /** @return true if more elements exist to be enumerated. */
        bool more() { return _pos < _theend; }

        /** @return true if more elements exist to be enumerated INCLUDING the EOO element which is always at the end. */
        bool moreWithEOO() { return _pos <= _theend; }

        /** @return the next element in the object. For the final element, element.eoo() will be true. */
        BSONElement next( bool checkEnd ) {
            verify( _pos <= _theend );
            
            int maxLen = -1;
            if ( checkEnd ) {
                maxLen = _theend + 1 - _pos;
                verify( maxLen > 0 );
            }

            BSONElement e( _pos, maxLen );
            int esize = e.size( maxLen );
            massert( 16446, "BSONElement has bad size", esize > 0 );
            _pos += esize;

            return e;
        }
        BSONElement next() {
            verify( _pos <= _theend );
            BSONElement e(_pos);
            _pos += e.size();
            return e;
        }
        void operator++() { next(); }
        void operator++(int) { next(); }

        BSONElement operator*() {
            verify( _pos <= _theend );
            return BSONElement(_pos);
        }

    private:
        const char* _pos;
        const char* _theend;
    };

    /** Base class implementing ordered iteration through BSONElements. */
    class BSONIteratorSorted {
        MONGO_DISALLOW_COPYING(BSONIteratorSorted);
    public:
        ~BSONIteratorSorted() {
            verify( _fields );
        }

        bool more() {
            return _cur < _nfields;
        }

        BSONElement next() {
            verify( _fields );
            if ( _cur < _nfields )
                return BSONElement( _fields[_cur++] );
            return BSONElement();
        }

    protected:
        class ElementFieldCmp;
        BSONIteratorSorted( const BSONObj &o, const ElementFieldCmp &cmp );
        
    private:
        const int _nfields;
        const boost::scoped_array<const char *> _fields;
        int _cur;
    };

    /** Provides iteration of a BSONObj's BSONElements in lexical field order. */
    class BSONObjIteratorSorted : public BSONIteratorSorted {
    public:
        BSONObjIteratorSorted( const BSONObj &object );
    };

    /**
     * Provides iteration of a BSONArray's BSONElements in numeric field order.
     * The elements of a bson array should always be numerically ordered by field name, but this
     * implementation re-sorts them anyway.
     */
    class BSONArrayIteratorSorted : public BSONIteratorSorted {
    public:
        BSONArrayIteratorSorted( const BSONArray &array );
    };

    /** Similar to BOOST_FOREACH
     *
     *  because the iterator is defined outside of the for, you must use {} around
     *  the surrounding scope. Don't do this:
     *
     *  if (foo)
     *      BSONForEach(e, obj)
     *          doSomething(e);
     *
     *  but this is OK:
     *
     *  if (foo) {
     *      BSONForEach(e, obj)
     *          doSomething(e);
     *  }
     *
     */

#define BSONForEach(e, obj)                                     \
    BSONObjIterator BOOST_PP_CAT(it_,__LINE__)(obj);            \
    for ( BSONElement e;                                        \
            (BOOST_PP_CAT(it_,__LINE__).more() ?                  \
             (e = BOOST_PP_CAT(it_,__LINE__).next(), true) :  \
             false) ;                                         \
            /*nothing*/ )

}
