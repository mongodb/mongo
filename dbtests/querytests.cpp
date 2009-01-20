// pdfiletests.cpp : query.{h,cpp} unit tests.
//

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

#include "../db/query.h"

#include "../db/db.h"
#include "../db/json.h"

#include "dbtests.h"

namespace QueryTests {

    class Base {
    public:
        Base() {
            dblock lk;
            setClient( ns() );
            addIndex( fromjson( "{\"a\":1}" ) );
        }
    protected:
        static const char *ns() {
            return "unittest.querytests";
        }
        static void addIndex( const BSONObj &key ) {
            BSONObjBuilder b;
            b.append( "name", "index" );
            b.append( "ns", ns() );
            b.append( "key", key );
            BSONObj o = b.done();
            stringstream indexNs;
            indexNs << ns() << ".system.indexes";
            theDataFileMgr.insert( indexNs.str().c_str(), o.objdata(), o.objsize() );
        }
    };
    
    class NoFindSpec : public Base {
    public:
        void run() {
            ASSERT( !getIndexCursor( ns(), emptyObj, emptyObj ).get() );
        }
    };
    
    class SimpleFind : public Base {
    public:
        void run() {
            bool simpleKeyMatch = false;
            bool isSorted = false;
            ASSERT( getIndexCursor( ns(), fromjson( "{\"a\":\"b\"}" ),
                                   emptyObj, &simpleKeyMatch ).get() );
            ASSERT( simpleKeyMatch );
            ASSERT( !isSorted );
        }
    };
    
    class SimpleFindSort : public Base {
    public:
        void run() {
            bool simpleKeyMatch = false;
            bool isSorted = false;
            ASSERT( getIndexCursor( ns(), fromjson( "{\"a\":\"b\"}" ),
                                    fromjson( "{\"a\":1}" ), &simpleKeyMatch,
                                    &isSorted ).get() );
            ASSERT( !simpleKeyMatch );
            ASSERT( isSorted );
        }        
    };
    
    class FindNumericNotSimple : public Base {
    public:
        void run() {
            bool simpleKeyMatch = false;
            bool isSorted = false;
            ASSERT( getIndexCursor( ns(), fromjson( "{\"a\":1}" ),
                                   emptyObj, &simpleKeyMatch,
                                   &isSorted ).get() );
            ASSERT( !simpleKeyMatch );
            ASSERT( !isSorted );
        }        
    };

    class FindObjectNotSimple : public Base {
    public:
        void run() {
            bool simpleKeyMatch = false;
            bool isSorted = false;
            ASSERT( getIndexCursor( ns(), fromjson( "{\"a\":{\"b\":1}}" ),
                                   emptyObj, &simpleKeyMatch,
                                   &isSorted ).get() );
            ASSERT( !simpleKeyMatch );
            ASSERT( !isSorted );
        }        
    };

    class All : public UnitTest::Suite {
    public:
        All() {
            add< NoFindSpec >();
            add< SimpleFind >();
            add< SimpleFindSort >();
            add< FindNumericNotSimple >();
            add< FindObjectNotSimple >();
        }
    };
    
} // namespace QueryTests

UnitTest::TestPtr queryTests() {
    return UnitTest::createSuite< QueryTests::All >();
}