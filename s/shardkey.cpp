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

#include "stdafx.h"
#include "shard.h"
#include "../db/jsobj.h"
#include "../util/unittest.h"

/**
   TODO: this only works with numbers right now
         this is very temporary, need to make work with anything
*/

namespace mongo {

    BSONObj ShardKeyPattern::globalMin() const {
        BSONObjBuilder b;
        BSONElement e = pattern.firstElement();
        b.appendMinKey(e.fieldName());
        return b.obj();
    }

    BSONObj ShardKeyPattern::globalMax() const {
        BSONObjBuilder b;
        BSONElement e = pattern.firstElement();
        b.appendMaxKey(e.fieldName());
        return b.obj();
    }

    int ShardKeyPattern::compare( const BSONObj& lObject , const BSONObj& rObject ) {
        BSONObj L = extractKey(lObject);
        uassert("left object doesn't have shard key", !L.isEmpty());
        BSONObj R = extractKey(rObject);
        uassert("right object doesn't have shard key", !R.isEmpty());
        return L.woCompare(R);
    }

    // return X / 2
    OID div2(OID X) { 
        unsigned char *x = (unsigned char *) &X;
        unsigned char carry = 0;
        for(int i = 0; i <= 11; i++ ) {
            unsigned char ncarry = (x[i] & 1) << 7;
            x[i] = (x[i] >> 1) | carry;
            carry = ncarry;
        }
        return X;
    }

    OID operator+(OID X, OID Y) { 
        OID Res;
        unsigned char *res = (unsigned char *) &Res;
        unsigned char *x = (unsigned char *) &X;
        unsigned char *y = (unsigned char *) &Y;

        int carry = 0;
        for( int i = 11; i >= 0; i-- ) {
            int a = x[i];
            int b = y[i];
            int c = a+b+carry;
            if( c >= 256 ) { 
                c -= 256;
                carry = 1;
            }
            else
                carry = 0;
            assert( c >= 0 && c <= 255 );
            res[i] = (unsigned char) c;
        }
        assert( carry == 0 );
        return Res;
    }

    // return X-Y
    OID operator-(OID X, OID Y) { 
        OID Res;
        unsigned char *res = (unsigned char *) &Res;
        unsigned char *x = (unsigned char *) &X;
        unsigned char *y = (unsigned char *) &Y;

        // oid's are in a big endian style order, so we do this byte by byte
        int carry = 0;
        for( int i = 11; i >= 0; i-- ) {
            int a = x[i];
            int b = y[i];
            int c = a-b-carry;
            if( c < 0 ) { 
                c += 256;
                carry = 1;
            }
            else
                carry = 0;
            assert( c >= 0 && c <= 255 );
            res[i] = (unsigned char) c;
        }
        assert( carry == 0 );
        return Res;
    }

    OID averageOIDs(const OID& left, const OID& right) { 
        // return add(left, div2(diff(right,left)));
        return left + div2(right-left);
    }

    string middleString(const char *_left, const char *_right) { 
        const unsigned char *left = (const unsigned char *) _left;
        const unsigned char *right = (const unsigned char *) _right;
        unsigned char *x = (unsigned char *) malloc(strlen(_left)+1);
        strcpy((char *) x, (char *) left);
        unsigned char *l = x;
        const unsigned char *r = right;

        while( *l == *r ) { 
            assert( *l ); // can't be exactly same string
            l++; r++; 
        }
        // note *l might be null e.g. "xxx\0", "xxxy..."
        assert( *r );
        int ch = (((int) *l) + ((int) *r)) / 2;
        string s = string((const char *) x, l-x) + (char) ch;
        while( s <= (const char *) left ) { 
            // we rounded down to the same old value.  keep adding chars
            int ch = (((int) *++l) + 255) / 2;
            s += ch;
            assert( s < (const char *) right );
        }

        // cout << '"' << left << '"' << ' ' << '"' << right <<  '"' << ' ' << '"' <<  s << '"' << endl;
        return s;
    }

    /* average two element's values, must be the same type 
       range is closed on the left and open on the right, so left must neever touch the 
       right value: if they get to be adjacent, the left value is returned.
    */
    void averageValues(BSONObjBuilder&b, int type, BSONElement& l, BSONElement& r) { 
        const char *fn = l.fieldName();
        switch( type ) {
        case MaxKey:
        case jstNULL:
        case MinKey:
        case EOO:
            massert("unexpected type in averageValues", false);
            break;
        case NumberDouble:
        {
            double x = l.number();
            double y = r.number();
            assert( y >= x );
            b.append(fn, x + (y - x)/2);
            break;
        }
        case String:
            b.append(fn, middleString(l.valuestr(), r.valuestr()));
            break;
        case jstOID:
            b.append(fn, averageOIDs(l.__oid(), r.__oid()));
            break;
        case Bool:
            b.append(l);
            break;
        case Date:
            {
                unsigned long long x = l.date();
                unsigned long long y = r.date();
                assert( y >= x );
                b.appendDate(fn, x + (y - x)/2);
                break;
            }
        case NumberInt:
            {
                int x = (int) l.number();
                int y = (int) r.number();
                assert( y >= x );
                b.append(fn, x + (y - x)/2);
                break;
            }
         default:
            {
                stringstream ss;
                ss << "BSON type " << type << " not yet supported in shard keys";
                uasserted( ss.str() );
            }
        }
    }

    int nextType(int t) { 
        switch( t ) { 
        case MinKey: return NumberDouble;
        case NumberDouble: return String;
        case String: return jstOID;
        case jstOID: return Bool;
        case Bool: return Date;
        case Date: return jstNULL;
        case jstNULL: return NumberInt;
        case NumberInt: return MaxKey;
        default:
            uassert("type not supported by sharding [nextType]", false);
        }
        return EOO;
    }

    BSONElement largestElementForType(int t, BSONObjBuilder& b) { 
        switch( t ) { 
        case MinKey: b.appendMinKey(""); break;
        case NumberDouble: b.append("", numeric_limits< double >::min()); break;
        case String: b.append("", ""); break;
        case jstOID: 
            { 
                OID o;
                memset(&o, 0, sizeof(o));
                b.appendOID("", &o);
                break;
            }
        case Bool: b.appendBool("", false); break;
        case Date: b.appendDate("", 0); break;
        case jstNULL: b.appendNull("");
        case NumberInt: b.append("", numeric_limits<int>::min()); break;
        default:
            uassert("type not supported by sharding [seft]", false);
        }
        return b.done().firstElement();
    }

    BSONElement smallestElementForType(int t, BSONObjBuilder& b) { 
        switch( t ) { 
        case MinKey: b.appendMinKey(""); break;
        case NumberDouble: b.append("", numeric_limits< double >::min()); break;
        case String: b.append("", ""); break;
        case jstOID: 
            { 
                OID o;
                memset(&o, 0, sizeof(o));
                b.appendOID("", &o);
                break;
            }
        case Bool: b.appendBool("", false); break;
        case Date: b.appendDate("", 0); break;
        case jstNULL: b.appendNull("");
        case NumberInt: b.append("", numeric_limits<int>::min()); break;
        default:
            uassert("type not supported by sharding [seft]", false);
        }
        return b.done().firstElement();
    }

    // assure l != r value before calling
    void middleVal(BSONObjBuilder& b, BSONElement& l, BSONElement& r) { 
        int lt = l.type();
        int rt = r.type();
        int d = rt - lt;
        assert( d >= 0 );

        if( d > 0 ) {
            int nextt = nextType(lt);
            BSONObjBuilder B, BB;
            BSONElement nexte = smallestElementForType(nextt, B);
            if( nextt == rt && compareElementValues(nexte, r) == 0 ) { 
                // too big.
                nexte = largestElementForType(nextt, BB);
            }
            b.appendAs(nexte, l.fieldName());
            return;
        }

        // same type
        averageValues(b, lt, l, r);
    }

    BSONObj ShardKeyPattern::middle( const BSONObj &lo , const BSONObj &ro ) {
        BSONObj L = extractKey(lo);
        BSONObj R = extractKey(ro);
        BSONElement l = L.firstElement();
        BSONElement r = R.firstElement();
        if( l == r || l.eoo() || r.eoo() )
            return L;

        BSONObjBuilder b;

        massert("not done for compound patterns", patternfields.size() == 1);

        middleVal(b, l, r);
        BSONObj res = b.obj();

        if( res.woEqual(ro) ) { 
            // range is minimal, i.e., two adjacent values.  as RHS is open, 
            // return LHS
            return lo;
        }

        /* compound:
        BSONObjIterator li(L);
        BSONObjIterator ri(R);
        while( 1 ) { 

        }
        */

        return res;
    }

    bool ShardKeyPattern::hasShardKey( const BSONObj& obj ) {
        /* this is written s.t. if obj has lots of fields, if the shard key fields are early, 
           it is fast.  so a bit more work to try to be semi-fast.
           */
        BSONObjIterator i(obj);
        int n = patternfields.size();
        while( 1 ) {
            BSONElement e = i.next();
            if( e.eoo() )
                return false;
            if( patternfields.find(e.fieldName()) == patternfields.end() )
                continue;
            if( --n == 0 ) 
                break;
        }
        return true;
    }

    /** @return true if shard s is relevant for query q.

    Example:
     q:     { x : 3 }
     *this: { x : 1 }
     s:     x:2..x:7
       -> true
    */

    bool ShardKeyPattern::relevant(const BSONObj& query, BSONObj& L, BSONObj& R) { 
        BSONObj q = extractKey( query );
        if( q.isEmpty() )
            return true;

        BSONElement e = q.firstElement();
        assert( !e.eoo() ) ;

        if( e.type() == RegEx ) {
            /* todo: if starts with ^, we could be smarter here */
            return true;
        }

        if( e.type() == Object ) { 
            BSONObjIterator j(e.embeddedObject());
            BSONElement LE = L.firstElement(); // todo compound keys
            BSONElement RE = R.firstElement(); // todo compound keys
            while( 1 ) { 
                BSONElement f = j.next();
                if( f.eoo() ) 
                    break;
                int op = f.getGtLtOp();
                switch( op ) { 
                    case JSMatcher::LT:
                        if( compareValues(f, LE) <= 0 )
                            return false;
                        break;
                    case JSMatcher::LTE:
                        if( compareValues(f, LE) < 0 )
                            return false;
                        break;
                    case JSMatcher::GT:
                    case JSMatcher::GTE:
                        if( compareValues(f, RE) >= 0 )
                            return false;
                        break;
                    case JSMatcher::opIN:
                    case JSMatcher::NE:
                    case JSMatcher::opSIZE:
                        massert("not implemented yet relevant()", false);
                    case JSMatcher::Equality:
                        goto normal;
                    default:
                        massert("bad operator in relevant()?", false);
                }
            }
            return true;
        }
normal:
        return L.woCompare(q) <= 0 && R.woCompare(q) > 0;
    }

    bool ShardKeyPattern::relevantForQuery( const BSONObj& query , Shard * shard ){
/*        if ( ! hasShardKey( query ) ){
            // if the shard key isn't in the query, then we have to go everywhere
            // therefore this shard is relevant
            return true;
        }
*/
        massert("not done for compound patterns", patternfields.size() == 1);

        bool rel = relevant(query, shard->getMin(), shard->getMax());
        if( !hasShardKey( query ) )
            assert(rel);

        return rel;

/*        if( e.type() == Object ) { 
			//			cout << "\n\nrfq\n" << v.toString() << "\n\nquery:\n" << query.toString() << endl;
			//			sleepsecs(99);
            massert( "gt/lt etc. support not done yet", e.embeddedObject().firstElement().fieldName()[0] != '$');
        }*/

        /* todo:
          _ $gt/$lt
          _ $ne 
          _ regex
        */
/*
        return
            compare( shard->getMin() , v ) <= 0 &&
            compare( v, shard->getMax() ) < 0;
*/
    }

    /**
      returns a query that filters results only for the range desired, i.e. returns 
        { $gte : keyval(min), $lt : keyval(max) }
    */
    void ShardKeyPattern::getFilter( BSONObjBuilder& b , const BSONObj& min, const BSONObj& max ){
        massert("not done for compound patterns", patternfields.size() == 1);
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
    int ShardKeyPattern::canOrder( const BSONObj& sort ){
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
            if( !p.more() ) 
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

    string ShardKeyPattern::toString() const {
        return pattern.toString();
    }

    class ShardKeyUnitTest : public UnitTest {
    public:
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
        void rfq() {
            ShardKeyPattern k( BSON( "key" << 1 ) );
            BSONObj q = BSON( "key" << 3 );
            Shard s(0);
            BSONObj z = fromjson("{ ns : \"alleyinsider.fs.chunks\" , min : {key:2} , max : {key:20} , server : \"localhost:30001\" }");
            s.unserialize(z);
            assert( k.relevantForQuery(q, &s) );
            assert( k.relevantForQuery(fromjson("{foo:9,key:4}"), &s) );
            assert( !k.relevantForQuery(fromjson("{foo:9,key:43}"), &s) );
            assert( k.relevantForQuery(fromjson("{foo:9,key:{$gt:10}}"), &s) );
            assert( !k.relevantForQuery(fromjson("{foo:9,key:{$gt:22}}"), &s) );
            assert( k.relevantForQuery(fromjson("{foo:9}"), &s) );
        }
        void getfilt() { 
            ShardKeyPattern k( BSON( "key" << 1 ) );
            BSONObjBuilder b;
            k.getFilter(b, fromjson("{z:3,key:30}"), fromjson("{key:90}"));
            BSONObj x = fromjson("{ key: { $gte: 30.0, $lt: 90.0 } }");
            assert( x.woEqual(b.obj()) );
        }
        void mid(const char *a, const char *b) { 
            ShardKeyPattern k( BSON( "key" << 1 ) );
            BSONObj A = fromjson(a);
            BSONObj B = fromjson(b);
            BSONObj x = k.middle(A, B);
            assert( A.woCompare(x) < 0 );
            assert( x.woCompare(B) < 0 );
        }
        void testMiddle() { 
            mid( "{key:10}", "{key:30}" );
            mid( "{key:10}", "{key:null}" );
            mid( "{key:\"Jane\"}", "{key:\"Tom\"}" );

            BSONObjBuilder b;
            b.appendMinKey("k");
            BSONObj min = b.obj();
            BSONObjBuilder b2;
            b2.appendMaxKey("k");
            BSONObj max = b2.obj();
            ShardKeyPattern k( BSON( "k" << 1 ) );
//            cout << min.toString() << endl;
            BSONObj m = k.middle(min, max);
//            cout << m << endl;
            BSONObj n = k.middle(m, max);
//            cout << n << endl;
            BSONObj p = k.middle(min, m);
//            cout << "\n" << min.toString() << " " << m.toString() << endl;
//            cout << p << endl;
        }
        void testGlobal(){
            ShardKeyPattern k( fromjson( "{num:1}" ) );
            DEV cout << "global middle:" << k.middle( k.globalMin() , k.globalMax() ) << endl;
        }
        void div(const char *a, const char *res) { 
            OID A,RES;
            A.init(a);
            RES = div2(A);
            assert( RES.str() == res );
        }
        void diff(const char *a, const char *b, const char *res) { 
            OID A,B,RES;
            A.init(a); B.init(b);
            RES = A - B;
            assert( RES.str() == res );

            assert( RES + B == A );
        }
        void oid() {
            diff("800000000000000000000000", "000000000000000000000001", "7fffffffffffffffffffffff");
            diff("800000000000000000000001", "800000000000000000000000", "000000000000000000000001");
            diff("800000000000000000000000", "800000000000000000000000", "000000000000000000000000");
            div("800000000000000000000000", "400000000000000000000000");
            div("010000000000000000000000", "008000000000000000000000");
        }
        void checkstr(const char *a, const char *b) { 
            string s = middleString(a, b);
            assert( s > a );
            assert( s < b );
        }
        void middlestrtest() { 
            checkstr("a\377", "b");
            checkstr("a\377\377", "b");
            checkstr("a", "b");
            checkstr("a","c");
            checkstr("aa","ac");
            checkstr("aac","ac");
            checkstr("aa","aat");
            checkstr("aat", "acq");
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
            oid();
            middlestrtest();

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

            assert( k.compare(a,k.middle(a,a)) == 0 );
            assert( k.compare(a,k.middle(a,b)) <= 0 );
            assert( k.compare(k.middle(a,b),b) <= 0 );

            assert( k.canOrder( fromjson("{key:1}") ) == 1 );
            assert( k.canOrder( fromjson("{zz:1}") ) == 0 );
            assert( k.canOrder( fromjson("{key:-1}") ) == -1 );
            
            testCanOrder();
            testMiddle();
            testGlobal();
            getfilt();
            rfq();
            // add middle multitype tests
        }
    } shardKeyTest;
    
} // namespace mongo
