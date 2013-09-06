// shardkey.cpp

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

#include "mongo/pch.h"

#include "mongo/s/chunk.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/timer.h"

namespace mongo {

    ShardKeyPattern::ShardKeyPattern( BSONObj p ) : pattern( p.getOwned() ) {
        pattern.toBSON().getFieldNames( patternfields );

        BSONObjBuilder min;
        BSONObjBuilder max;

        BSONObjIterator it(p);
        while (it.more()) {
            BSONElement e (it.next());
            min.appendMinKey(e.fieldName());
            max.appendMaxKey(e.fieldName());
        }

        gMin = min.obj();
        gMax = max.obj();
    }

    bool ShardKeyPattern::hasShardKey( const BSONObj& obj ) const {
        /* this is written s.t. if obj has lots of fields, if the shard key fields are early,
           it is fast.  so a bit more work to try to be semi-fast.
           */

        for(set<string>::const_iterator it = patternfields.begin(); it != patternfields.end(); ++it) {
            BSONElement e = obj.getFieldDotted(it->c_str());
            if(     e.eoo() ||
                    e.type() == Array ||
                    (e.type() == Object && !e.embeddedObject().okForStorage())) {
                // Don't allow anything for a shard key we can't store -- like $gt/$lt ops
                return false;
            }
        }
        return true;
    }

    bool ShardKeyPattern::isPrefixOf( const KeyPattern& otherPattern ) const {
        return pattern.isPrefixOf( otherPattern );
    }

    bool ShardKeyPattern::isUniqueIndexCompatible( const KeyPattern& uniqueIndexPattern ) const {
        if ( ! uniqueIndexPattern.toBSON().isEmpty() &&
             str::equals( uniqueIndexPattern.toBSON().firstElementFieldName(), "_id" ) ){
            return true;
        }
        return pattern.toBSON().isFieldNamePrefixOf( uniqueIndexPattern.toBSON() );
    }

    string ShardKeyPattern::toString() const {
        return pattern.toString();
    }

    BSONObj ShardKeyPattern::moveToFront(const BSONObj& obj) const {
        vector<const char*> keysToMove;
        keysToMove.push_back("_id");
        BSONForEach(e, pattern.toBSON()) {
            if (strchr(e.fieldName(), '.') == NULL && strcmp(e.fieldName(), "_id") != 0)
                keysToMove.push_back(e.fieldName());
        }

        if (keysToMove.size() == 1) {
            return obj;

        }
        else {
            BufBuilder buf (obj.objsize());
            buf.appendNum((unsigned)0); // refcount
            buf.appendNum(obj.objsize());

            vector<pair<const char*, size_t> > copies;
            pair<const char*, size_t> toCopy ((const char*)NULL, 0); // C++ NULL isn't a pointer type yet

            BSONForEach(e, obj) {
                bool moveToFront = false;
                for (vector<const char*>::const_iterator it(keysToMove.begin()), end(keysToMove.end()); it!=end; ++it) {
                    if (strcmp(e.fieldName(), *it) == 0) {
                        moveToFront = true;
                        break;
                    }
                }

                if (moveToFront) {
                    buf.appendBuf(e.fieldName()-1, e.size());
                    if (toCopy.first) {
                        copies.push_back(toCopy);
                        toCopy.first = NULL;
                    }
                }
                else {
                    if (!toCopy.first) {
                        toCopy.first = e.fieldName()-1;
                        toCopy.second = e.size();
                    }
                    else {
                        toCopy.second += e.size();
                    }
                }
            }

            for (vector<pair<const char*, size_t> >::const_iterator it(copies.begin()), end(copies.end()); it!=end; ++it) {
                buf.appendBuf(it->first, it->second);
            }

            if (toCopy.first) {
                buf.appendBuf(toCopy.first, toCopy.second);
            }

            buf.appendChar('\0');

            BSONObj out ((BSONObj::Holder*)buf.buf());
            buf.decouple();
            return out;
        }
    }

    /* things to test for compound :
       \ middle (deprecating?)
    */
    class ShardKeyUnitTest : public StartupTest {
    public:

        void hasshardkeytest() {
            ShardKeyPattern k( BSON( "num" << 1 ) );

            BSONObj x = fromjson("{ zid : \"abcdefg\", num: 1.0, name: \"eliot\" }");
            verify( k.hasShardKey(x) );
            verify( !k.hasShardKey( fromjson("{foo:'a'}") ) );
            verify( !k.hasShardKey( fromjson("{x: {$gt: 1}}") ) );
            verify( !k.hasShardKey( fromjson("{num: {$gt: 1}}") ) );
            BSONObj obj = BSON( "num" << BSON( "$ref" << "coll" << "$id" << 1));
            verify( k.hasShardKey(obj));

            // try compound key
            {
                ShardKeyPattern k( fromjson("{a:1,b:-1,c:1}") );
                verify( k.hasShardKey( fromjson("{foo:'a',a:'b',c:'z',b:9,k:99}") ) );
                BSONObj obj = BSON( "foo" << "a" <<
                                    "a" << BSON("$ref" << "coll" << "$id" << 1) <<
                                    "c" << 1 << "b" << 9 << "k" << 99 );
                verify( k.hasShardKey(  obj ) );
                verify( !k.hasShardKey( fromjson("{foo:'a',a:[1,2],c:'z',b:9,k:99}") ) );
                verify( !k.hasShardKey( fromjson("{foo:'a',a:{$gt:1},c:'z',b:9,k:99}") ) );
                verify( !k.hasShardKey( fromjson("{foo:'a',a:'b',c:'z',bb:9,k:99}") ) );
                verify( !k.hasShardKey( fromjson("{k:99}") ) );
            }

            // try dotted key
            {
                ShardKeyPattern k( fromjson("{'a.b':1}") );
                verify( k.hasShardKey( fromjson("{a:{b:1,c:1},d:1}") ) );
                verify( k.hasShardKey( fromjson("{'a.b':1}") ) );
                BSONObj obj = BSON( "c" << "a" <<
                                    "a" << BSON("$ref" << "coll" << "$id" << 1) );
                verify( !k.hasShardKey(  obj ) );
                obj = BSON( "c" << "a" <<
                            "a" << BSON( "b" << BSON("$ref" << "coll" << "$id" << 1) <<
                                         "c" << 1));
                verify( k.hasShardKey(  obj ) );
                verify( !k.hasShardKey( fromjson("{'a.c':1}") ) );
                verify( !k.hasShardKey( fromjson("{'a':[{b:1}, {c:1}]}") ) );
                verify( !k.hasShardKey( fromjson("{a:{b:[1,2]},d:1}") ) );
                verify( !k.hasShardKey( fromjson("{a:{c:1},d:1}") ) );
                verify( !k.hasShardKey( fromjson("{a:1}") ) );
                verify( !k.hasShardKey( fromjson("{b:1}") ) );
            }

        }

        void extractkeytest() {
            ShardKeyPattern k( fromjson("{a:1,'sub.b':-1,'sub.c':1}") );

            BSONObj x = fromjson("{a:1,'sub.b':2,'sub.c':3}");
            verify( k.extractKey( fromjson("{a:1,sub:{b:2,c:3}}") ).binaryEqual(x) );
            verify( k.extractKey( fromjson("{sub:{b:2,c:3},a:1}") ).binaryEqual(x) );
        }

        void isSpecialTest() {
            ShardKeyPattern k1( BSON( "a" << 1) );
            verify( ! k1.isSpecial() );

            ShardKeyPattern k2( BSON( "a" << -1 << "b" << 1 ) );
            verify( ! k2.isSpecial() );

            ShardKeyPattern k3( BSON( "a" << "hashed") );
            verify( k3.isSpecial() );

            ShardKeyPattern k4( BSON( "a" << 1 << "b" << "hashed") );
            verify( k4.isSpecial() );
        }

        void moveToFrontTest() {
            ShardKeyPattern sk (BSON("a" << 1 << "b" << 1));

            BSONObj ret;

            ret = sk.moveToFront(BSON("z" << 1 << "_id" << 1 << "y" << 1 << "a" << 1 << "x" << 1 << "b" << 1 << "w" << 1));
            verify(ret.binaryEqual(BSON("_id" << 1 << "a" << 1 << "b" << 1 << "z" << 1 << "y" << 1 << "x" << 1 << "w" << 1)));

            ret = sk.moveToFront(BSON("_id" << 1 << "a" << 1 << "b" << 1 << "z" << 1 << "y" << 1 << "x" << 1 << "w" << 1));
            verify(ret.binaryEqual(BSON("_id" << 1 << "a" << 1 << "b" << 1 << "z" << 1 << "y" << 1 << "x" << 1 << "w" << 1)));

            ret = sk.moveToFront(BSON("z" << 1 << "y" << 1 << "a" << 1 << "b" << 1 << "Z" << 1 << "Y" << 1));
            verify(ret.binaryEqual(BSON("a" << 1 << "b" << 1 << "z" << 1 << "y" << 1 << "Z" << 1 << "Y" << 1)));

        }

        void uniqueIndexCompatibleTest() {
            ShardKeyPattern k1( BSON( "a" << 1 ) );
            verify( k1.isUniqueIndexCompatible( BSON( "_id" << 1 ) ) );
            verify( k1.isUniqueIndexCompatible( BSON( "a" << 1 << "b" << 1 ) ) );
            verify( k1.isUniqueIndexCompatible( BSON( "a" << -1 ) ) );
            verify( ! k1.isUniqueIndexCompatible( BSON( "b" << 1 ) ) );

            ShardKeyPattern k2( BSON( "a" <<  "hashed") );
            verify( k2.isUniqueIndexCompatible( BSON( "a" << 1 ) ) );
            verify( ! k2.isUniqueIndexCompatible( BSON( "b" << 1 ) ) );
        }

        void moveToFrontBenchmark(int numFields) {
            BSONObjBuilder bb;
            bb.append("_id", 1);
            for (int i=0; i < numFields; i++)
                bb.append(BSONObjBuilder::numStr(i), 1);
            bb.append("key", 1);
            BSONObj o = bb.obj();

            ShardKeyPattern sk (BSON("key" << 1));

            Timer t;
            const int iterations = 100*1000;
            for (int i=0; i< iterations; i++) {
                sk.moveToFront(o);
            }

            const double secs = t.micros() / 1000000.0;
            const double ops_per_sec = iterations / secs;

            cout << "moveToFront (" << numFields << " fields) secs: " << secs << " ops_per_sec: " << ops_per_sec << endl;
        }
        void run() {
            extractkeytest();

            ShardKeyPattern k( BSON( "key" << 1 ) );

            BSONObj min = k.globalMin();

//            cout << min.jsonString(TenGen) << endl;

            BSONObj max = k.globalMax();

            BSONObj k1 = BSON( "key" << 5 );

            verify( min < max );
            verify( min < k.extractKey( k1 ) );
            verify( max > min );

            hasshardkeytest();
            verify( k.hasShardKey( k1 ) );
            verify( ! k.hasShardKey( BSON( "key2" << 1 ) ) );

            BSONObj a = k1;
            BSONObj b = BSON( "key" << 999 );

            verify( k.extractKey( a ) <  k.extractKey( b ) );

            isSpecialTest();

            // add middle multitype tests

            moveToFrontTest();

            uniqueIndexCompatibleTest();

            if (0) { // toggle to run benchmark
                moveToFrontBenchmark(0);
                moveToFrontBenchmark(10);
                moveToFrontBenchmark(100);
            }

            LOG(1) << "shardKeyTest passed" << endl;
        }
    } shardKeyTest;

} // namespace mongo
