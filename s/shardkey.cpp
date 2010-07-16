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
*/

#include "pch.h"
#include "chunk.h"
#include "../db/jsobj.h"
#include "../util/unittest.h"

namespace mongo {

    ShardKeyPattern::ShardKeyPattern( BSONObj p ) : pattern( p.getOwned() ) {
        pattern.getFieldNames(patternfields);

        BSONObjBuilder min;
        BSONObjBuilder max;

        BSONObjIterator it(p);
        while (it.more()){
            BSONElement e (it.next());
            min.appendMinKey(e.fieldName());
            max.appendMaxKey(e.fieldName());
        }
        
        gMin = min.obj();
        gMax = max.obj();
    }

    int ShardKeyPattern::compare( const BSONObj& lObject , const BSONObj& rObject ) const {
        BSONObj L = extractKey(lObject);
        uassert( 10198 , "left object doesn't have full shard key", L.nFields() == (int)patternfields.size());
        BSONObj R = extractKey(rObject);
        uassert( 10199 , "right object doesn't have full shard key", R.nFields() == (int)patternfields.size());
        return L.woCompare(R);
    }

    bool ShardKeyPattern::hasShardKey( const BSONObj& obj ) const {
        /* this is written s.t. if obj has lots of fields, if the shard key fields are early, 
           it is fast.  so a bit more work to try to be semi-fast.
           */

        for(set<string>::const_iterator it = patternfields.begin(); it != patternfields.end(); ++it){
            if(obj.getFieldDotted(it->c_str()).eoo())
                return false;
        }
        return true;
    }

    bool ShardKeyPattern::isPrefixOf( const BSONObj& otherPattern ) const {
        BSONObjIterator a( pattern );
        BSONObjIterator b( otherPattern );
        
        while ( a.more() && b.more() ){
            BSONElement x = a.next();
            BSONElement y = b.next();
            if ( strcmp( x.fieldName() , y.fieldName() ) )
                return false;
        }
        
        return ! a.more();
    }
    
    string ShardKeyPattern::toString() const {
        return pattern.toString();
    }
    
    /* things to test for compound : 
       \ middle (deprecating?)
    */
    class ShardKeyUnitTest : public UnitTest {
    public:
        
        void testIsPrefixOf(){
            {
                ShardKeyPattern k( BSON( "x" << 1 ) );
                assert( ! k.isPrefixOf( BSON( "a" << 1 ) ) );
                assert( k.isPrefixOf( BSON( "x" << 1 ) ) );
                assert( k.isPrefixOf( BSON( "x" << 1 << "a" << 1 ) ) );
                assert( ! k.isPrefixOf( BSON( "a" << 1 << "x" << 1 ) ) );
            }
            { 
                ShardKeyPattern k( BSON( "x" << 1 << "y" << 1 ) );
                assert( ! k.isPrefixOf( BSON( "x" << 1 ) ) );
                assert( ! k.isPrefixOf( BSON( "x" << 1 << "z" << 1 ) ) );
                assert( k.isPrefixOf( BSON( "x" << 1 << "y" << 1 ) ) );
                assert( k.isPrefixOf( BSON( "x" << 1 << "y" << 1 << "z" << 1 ) ) );
            }
        }
        
        void hasshardkeytest() { 
            BSONObj x = fromjson("{ zid : \"abcdefg\", num: 1.0, name: \"eliot\" }");
            ShardKeyPattern k( BSON( "num" << 1 ) );
            assert( k.hasShardKey(x) );
            assert( !k.hasShardKey( fromjson("{foo:'a'}") ) );

            // try compound key
            {
                ShardKeyPattern k( fromjson("{a:1,b:-1,c:1}") );
                assert( k.hasShardKey( fromjson("{foo:'a',a:'b',c:'z',b:9,k:99}") ) );
                assert( !k.hasShardKey( fromjson("{foo:'a',a:'b',c:'z',bb:9,k:99}") ) );
                assert( !k.hasShardKey( fromjson("{k:99}") ) );
            }

        }

        void extractkeytest() { 
            ShardKeyPattern k( fromjson("{a:1,'sub.b':-1,'sub.c':1}") );

            BSONObj x = fromjson("{a:1,'sub.b':2,'sub.c':3}");
            assert( k.extractKey( fromjson("{a:1,sub:{b:2,c:3}}") ).woEqual(x) );
            assert( k.extractKey( fromjson("{sub:{b:2,c:3},a:1}") ).woEqual(x) );
        }
        void run(){
            extractkeytest();

            ShardKeyPattern k( BSON( "key" << 1 ) );
            
            BSONObj min = k.globalMin();

//            cout << min.jsonString(TenGen) << endl;

            BSONObj max = k.globalMax();
            
            BSONObj k1 = BSON( "key" << 5 );

            assert( k.compare( min , max ) < 0 );
            assert( k.compare( min , k1 ) < 0 );
            assert( k.compare( max , min ) > 0 );
            assert( k.compare( min , min ) == 0 );
            
            hasshardkeytest();
            assert( k.hasShardKey( k1 ) );
            assert( ! k.hasShardKey( BSON( "key2" << 1 ) ) );

            BSONObj a = k1;
            BSONObj b = BSON( "key" << 999 );

            assert( k.compare(a,b) < 0 );
            
            testIsPrefixOf();
            // add middle multitype tests

            log(1) << "shardKeyTest passed" << endl;
        }
    } shardKeyTest;
    
} // namespace mongo
