// @file key.cpp

/**
*    Copyright (C) 2011 10gen Inc.
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
#include "key.h"

namespace mongo {

    // [ISKEY][HASMORE][x][y][canontype_4bits]

    /* warning: don't do BinData here unless you are careful with Geo, as geo 
                uses bindata.  you would want to perf test it on the change as 
                the geo code doesn't use Key's but rather BSONObj's for its 
                key manipulation.
                */

    enum CanonicalsEtc { 
        cminkey=1,
        cnull=2,
        cdouble=4,
        cstring=6,
        coid=8,
        cfalse=10,
        ctrue=11,
        cdate=12,
        cmaxkey=14,
        cCANONTYPEMASK = 0xf,
        cY = 0x10,
        cint = cY | cdouble,
        cX = 0x20,
        clong = cX | cdouble,
        cHASMORE = 0x40,
        cISKEY = 0x80
    };

    // fromBSON to Key format
    KeyV1Owned::KeyV1Owned(const BSONObj& obj) {
        BufBuilder b(512);
        BSONObj::iterator i(obj);
        assert( i.more() );
        unsigned char bits = cISKEY;
        while( 1 ) { 
            BSONElement e = i.next();
            if( i.more() )
                bits |= cHASMORE;
            switch( e.type() ) { 
            case MinKey:
                b.appendUChar(cminkey|bits);
                break;
            case jstNULL:
                b.appendUChar(cnull|bits);
                break;
            case MaxKey:
                b.appendUChar(cmaxkey|bits);
                break;
            case Bool:
                b.appendUChar( (e.boolean()?ctrue:cfalse) | bits );
                break;
            case jstOID:
                b.appendUChar(coid|bits);
                b.appendBuf(&e.__oid(), sizeof(OID));
                break;
            case Date:
                b.appendUChar(cdate|bits);
                b.appendStruct(e.date());
                break;
            case String:
                {
                    b.appendUChar(cstring|bits);
                    // should we do e.valuestrsize()-1?  last char currently will always be null.
                    unsigned x = (unsigned) e.valuestrsize();
                    if( x > 255 ) { 
                        _o = obj;
                        return;
                    }
                    b.appendUChar(x);
                    b.appendBuf(e.valuestr(), x);
                    break;
                }
            case NumberInt:
                b.appendUChar(cint|bits);
                b.appendNum((double) e._numberInt());
                break;
            case NumberLong:
                {
                    long long n = e._numberLong();
                    double d = (double) n;
                    if( d != n ) { 
                        _o = obj;
                        return;
                    }
                    b.appendUChar(clong|bits);
                    b.appendNum(d);
                    break;
                }
            case NumberDouble:
                {
                    double d = e._numberDouble();
                    bool nan = !( d <= numeric_limits< double >::max() &&
                        d >= -numeric_limits< double >::max() );
                    if( !nan ) { 
                        b.appendUChar(cdouble|bits);
                        b.appendNum(d);
                        break;
                    }
                    // else fall through and return a traditional BSON obj so our compressed keys need not check for nan
                }
            default:
                // if other types involved, store as traditional BSON
                _o = obj;
                return;
            }
            if( !i.more() )
                break;
            bits = 0;
        }
        _keyData = (const unsigned char *) b.buf();
        dassert( b.len() == dataSize() ); // check datasize method is correct
        b.decouple();
    }

    BSONObj KeyV1::toBson() const { 
        if( _keyData == 0 )
            return _o;

        BSONObjBuilder b(512);
        const unsigned char *p = _keyData;
        while( 1 ) { 
            unsigned bits = *p++;

            switch( bits & 0x3f ) {
                case cminkey: b.appendMinKey(""); break;
                case cnull:   b.appendNull(""); break;
                case cfalse:  b.appendBool("", false); break;
                case ctrue:   b.appendBool("", true); break;
                case cmaxkey: b.appendMaxKey(""); break;
                case cstring:
                    {
                        unsigned sz = *p++;
                        b.append("", (const char *) p, sz);
                        p += sz;
                        break;
                    }
                case coid:
                    b.appendOID("", (OID *) p);
                    p += sizeof(OID);
                    break;
                case cdate:
                    b.appendDate("", (Date_t&) *p);
                    p += 8;
                    break;
                case cdouble:
                    b.append("", (double&) *p);
                    p += sizeof(double);
                    break;
                case cint:
                    b.append("", (int) ((double&) *p));
                    p += sizeof(double);
                    break;
                case clong:
                    b.append("", (long long) ((double&) *p));
                    p += sizeof(double);
                    break;
                default:
                    assert(false);
            }

            if( (bits & cHASMORE) == 0 )
                break;
        }
        return b.obj();
    }

    static int compare(const unsigned char *&l, const unsigned char *&r) { 
        int lt = (*l & cCANONTYPEMASK);
        int rt = (*r & cCANONTYPEMASK);
        int x = lt - rt;
        if( x ) 
            return x;

        l++; r++;

        // same type
        switch( lt ) { 
        case cdouble:
            {
                double L = *((double *) l);
                double R = *((double *) r);
                if( L < R )
                    return -1;
                if( L > R )
                    return 1;
                l += 8; r += 8;
                break;
            }
        case cstring:
            {
                l++; r++; // skip the size byte
                // todo: see https://jira.mongodb.org/browse/SERVER-1300
                int res = strcmp((const char *) l, (const char *) r);
                if( res ) 
                    return res;
                unsigned sz = l[-1];
                l += sz; r += sz;
                break;
            }
        case coid:
            {
                int res = memcmp(l, r, sizeof(OID));
                if( res ) 
                    return res;
                l += 12; r += 12;
                break;
            }
        case cdate:
            {
                long long L = *((long long *) l);
                long long R = *((long long *) r);
                if( L < R )
                    return -1;
                if( L > R )
                    return 1;
                l += 8; r += 8;
                break;
            }
        default:
            // all the others are a match -- e.g. null == null
            ;
        }

        return 0;
    }

    // at least one of this and right are traditional BSON format
    int NOINLINE_DECL KeyV1::compareHybrid(const KeyV1& right, const Ordering& order) const { 
        BSONObj L = _keyData == 0 ? _o : toBson();
        BSONObj R = right._keyData == 0 ? right._o : right.toBson();
        return L.woCompare(R, order, /*considerfieldname*/false);
    }

    int KeyV1::woCompare(const KeyV1& right, const Ordering &order) const {
        const unsigned char *l = _keyData;
        const unsigned char *r = right._keyData;

        if( l==0 || r== 0 )
            return compareHybrid(right, order);

        unsigned mask = 1;
        while( 1 ) { 
            char lval = *l; 
            char rval = *r;
            {
                int x = compare(l, r); // updates l and r pointers
                if( x ) {
                    if( order.descending(mask) )
                        x = -x;
                    return x;
                }
            }

            {
                int x = ((int)(lval & cHASMORE)) - ((int)(rval & cHASMORE));
                if( x ) 
                    return x;
                if( (lval & cHASMORE) == 0 )
                    break;
            }

            mask <<= 1;
        }

        return 0;
    }

    bool KeyV1::woEqual(const KeyV1& right) const {
        const unsigned char *l = _keyData;
        const unsigned char *r = right._keyData;

        if( l==0 || r==0 ) {
            BSONObj L = _keyData == 0 ? _o : toBson();
            BSONObj R = right._keyData == 0 ? right._o : right.toBson();
            return L.woEqual(R);
        }

        while( 1 ) { 
            char lval = *l; 
            char rval = *r;
            if( compare(l, r) ) // updates l and r pointers
                return false;
            if( (lval&cHASMORE)^(rval&cHASMORE) )
                return false;
            if( (lval&cHASMORE) == 0 )
                break;
        }

        return true;
    }

    static unsigned sizes[] = {
        0,
        1, //cminkey=1,
        1, //cnull=2,
        0,
        9, //cdouble=4,
        0,
        0, //cstring=6,
        0,
        13, //coid=8,
        0,
        1, //cfalse=10,
        1, //ctrue=11,
        9, //cdate=12,
        0,
        1, //cmaxkey=14,
        0
    };

    int KeyV1::dataSize() const { 
        const unsigned char *p = _keyData;
        if( p == 0 )
            return _o.objsize();

        bool more;
        do { 
            unsigned type = *p & cCANONTYPEMASK;
            unsigned z = sizes[type];
            if( z == 0 ) {
                assert( type == cstring );
                z = ((unsigned) p[1]) + 2;
            }
            more = (*p & cHASMORE) != 0;
            p += z;
        } while( more );
        return p - _keyData;
    }

}
