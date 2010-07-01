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

/**
   TODO: this only works with numbers right now
         this is very temporary, need to make work with anything
*/

namespace mongo {
    void minForPat(BSONObjBuilder& out, const BSONObj& pat){
        BSONElement e = pat.firstElement();
        if (e.type() == Object){
            BSONObjBuilder sub;
            minForPat(sub, e.embeddedObject());
            out.append(e.fieldName(), sub.obj());
        } else {
            out.appendMinKey(e.fieldName());
        }
    }

    void maxForPat(BSONObjBuilder& out, const BSONObj& pat){
        BSONElement e = pat.firstElement();
        if (e.type() == Object){
            BSONObjBuilder sub;
            maxForPat(sub, e.embeddedObject());
            out.append(e.fieldName(), sub.obj());
        } else {
            out.appendMaxKey(e.fieldName());
        }
    }

    ShardKeyPattern::ShardKeyPattern( BSONObj p ) : pattern( p.getOwned() ) {
        pattern.getFieldNames(patternfields);

        BSONObjBuilder min;
        minForPat(min, pattern);
        gMin = min.obj();

        BSONObjBuilder max;
        maxForPat(max, pattern);
        gMax = max.obj();
    }

    int ShardKeyPattern::compare( const BSONObj& lObject , const BSONObj& rObject ) const {
        BSONObj L = extractKey(lObject);
        uassert( 10198 , "left object doesn't have shard key", !L.isEmpty());
        BSONObj R = extractKey(rObject);
        uassert( 10199 , "right object doesn't have shard key", !R.isEmpty());
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

    /**
      returns a query that filters results only for the range desired, i.e. returns 
        { $gte : keyval(min), $lt : keyval(max) }
    */
    void ShardKeyPattern::getFilter( BSONObjBuilder& b , const BSONObj& min, const BSONObj& max ) const{
        massert( 10426 , "not done for compound patterns", patternfields.size() == 1);
        BSONObjBuilder temp;
        temp.appendAs( extractKey(min).firstElement(), "$gte" );
        temp.appendAs( extractKey(max).firstElement(), "$lt" );

        b.append( patternfields.begin()->c_str(), temp.obj() );
    }    

    /**
      Example
      sort:   { ts: -1 }
      *this:  { ts:1 }
      -> -1

      @return
      0 if sort either doesn't have all the fields or has extra fields
      < 0 if sort is descending
      > 1 if sort is ascending
    */
    int ShardKeyPattern::canOrder( const BSONObj& sort ) const{
        // e.g.:
        //   sort { a : 1 , b : -1 }
        //   pattern { a : -1, b : 1, c : 1 }
        //     -> -1

        int dir = 0;

        BSONObjIterator s(sort);
        BSONObjIterator p(pattern);
        while( 1 ) {
            BSONElement e = s.next();
            if( e.eoo() )
                break;
            if( !p.moreWithEOO() ) 
                return 0;
            BSONElement ep = p.next();
            bool same = e == ep;
            if( !same ) {
                if( strcmp(e.fieldName(), ep.fieldName()) != 0 )
                    return 0;
                // same name, but opposite direction
                if( dir == -1 ) 
                    ;  // ok
                else if( dir == 1 )
                    return 0; // wrong direction for a 2nd field
                else // dir == 0, initial pass
                    dir = -1;
            }
            else { 
                // fields are the same
                if( dir == -1 ) 
                    return 0; // wrong direction
                dir = 1;
            }
        }

        return dir;
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
       x hasshardkey 
       _ getFilter (hard?)
       x canOrder
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
        void getfilt() { 
            ShardKeyPattern k( BSON( "key" << 1 ) );
            BSONObjBuilder b;
            k.getFilter(b, fromjson("{z:3,key:30}"), fromjson("{key:90}"));
            BSONObj x = fromjson("{ key: { $gte: 30, $lt: 90 } }");
            assert( x.woEqual(b.obj()) );
        }
        void testCanOrder() { 
            ShardKeyPattern k( fromjson("{a:1,b:-1,c:1}") );
            assert( k.canOrder( fromjson("{a:1}") ) == 1 );
            assert( k.canOrder( fromjson("{a:-1}") ) == -1 );
            assert( k.canOrder( fromjson("{a:1,b:-1,c:1}") ) == 1 );
            assert( k.canOrder( fromjson("{a:1,b:1}") ) == 0 );
            assert( k.canOrder( fromjson("{a:-1,b:1}") ) == -1 );
        }
        void extractkeytest() { 
            ShardKeyPattern k( fromjson("{a:1,b:-1,c:1}") );

            BSONObj x = fromjson("{a:1,b:2,c:3}");
            assert( k.extractKey( fromjson("{a:1,b:2,c:3}") ).woEqual(x) );
            assert( k.extractKey( fromjson("{b:2,c:3,a:1}") ).woEqual(x) );
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
            
            assert( k.canOrder( fromjson("{key:1}") ) == 1 );
            assert( k.canOrder( fromjson("{zz:1}") ) == 0 );
            assert( k.canOrder( fromjson("{key:-1}") ) == -1 );
            
            testCanOrder();
            getfilt();
            testIsPrefixOf();
            // add middle multitype tests

            log(1) << "shardKeyTest passed" << endl;
        }
    } shardKeyTest;
    
} // namespace mongo
