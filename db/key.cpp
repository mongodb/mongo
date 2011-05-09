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
#include "../util/unittest.h"

namespace mongo {

    // [ ][HASMORE][x][y][canontype_4bits]
    enum CanonicalsEtc { 
        cminkey=1,
        cnull=2,
        cdouble=4,
        cstring=6,
        cbindata=7,
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
        cNOTUSED = 0x80 // but see IsBSON sentinel - this bit not usable without great care
    };

    // bindata bson type
    const unsigned BinDataLenMask = 0xf0;  // lengths are powers of 2 of this value
    const unsigned BinDataTypeMask = 0x0f; // 0-7 as you would expect, 8-15 are 128+value.  see BinDataType.
    const int BinDataLenMax = 32;
    const int BinDataLengthToCode[] = { 
        0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 
        0x80, -1/*9*/, 0x90/*10*/, -1/*11*/, 0xa0/*12*/, -1/*13*/, 0xb0/*14*/, -1/*15*/,
        0xc0/*16*/, -1, -1, -1, 0xd0/*20*/, -1, -1, -1, 
        0xe0/*24*/, -1, -1, -1, -1, -1, -1, -1, 
        0xf0/*32*/ 
    };
    const int BinDataCodeToLength[] = { 
        0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 32
    };

    int binDataCodeToLength(int codeByte) { 
        return BinDataCodeToLength[codeByte >> 4];
    }

    /** object cannot be represented in compact format.  so store in traditional bson format 
        with a leading sentinel byte IsBSON to indicate it's in that format.

        Given that the KeyV1Owned constructor already grabbed a bufbuilder, we reuse it here 
        so that we don't have to do an extra malloc.
    */
    void KeyV1Owned::traditional(BufBuilder& b, const BSONObj& obj) { 
        b.reset();
        b.appendUChar(IsBSON);
        b.appendBuf(obj.objdata(), obj.objsize());
        _toFree = b.buf();
        _keyData = (const unsigned char *) _toFree;
        b.decouple();
    }

    // fromBSON to Key format
    KeyV1Owned::KeyV1Owned(const BSONObj& obj) {
        BufBuilder b(512);
        BSONObj::iterator i(obj);
        assert( i.more() );
        unsigned char bits = 0;
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
            case BinData:
                {
                    int t = e.binDataType();
                    // 0-7 and 0x80 to 0x87 are supported by Key
                    if( (t & 0x78) == 0 && t != ByteArrayDeprecated ) {
                        int len;
                        const char * d = e.binData(len);
                        int code = BinDataLengthToCode[len];
                        if( code >= 0 ) {
                            if( t >= 128 )
                                t = (t-128) | 0x08;
                            dassert( (code&t) == 0 );
                            b.appendUChar( cbindata|bits );
                            b.appendUChar( code | t );
                            b.appendBuf(d, len);
                            break;
                        }
                    }
                    traditional(b, obj);
                    return;
                }
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
                        traditional(b, obj);
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
                        traditional(b, obj);
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
                traditional(b, obj);
                return;
            }
            if( !i.more() )
                break;
            bits = 0;
        }
        _toFree = b.buf();
        _keyData = (const unsigned char *) _toFree;
        dassert( b.len() == dataSize() ); // check datasize method is correct
        dassert( (*_keyData & cNOTUSED) == 0 );
        b.decouple();
    }

    BSONObj KeyV1::toBson() const { 
        if( !isCompactFormat() )
            return bson();

        BSONObjBuilder b(512);
        const unsigned char *p = _keyData;
        while( 1 ) { 
            unsigned bits = *p++;

            switch( bits & 0x3f ) {
                case cminkey: b.appendMinKey(""); break;
                case cnull:   b.appendNull(""); break;
                case cfalse:  b.appendBool("", false); break;
                case ctrue:   b.appendBool("", true); break;
                case cmaxkey: 
                    b.appendMaxKey(""); 
                    break;
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
                case cbindata:
                    {
                        int len = binDataCodeToLength(*p);
                        int subtype = (*p) & BinDataTypeMask;
                        if( subtype & 0x8 ) { 
                            subtype = (subtype & 0x7) | 0x80;
                        }
                        b.appendBinData("", len, (BinDataType) subtype, ++p);
                        p += len;
                        break;
                    }
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
                unsigned sz = *l;
                l++; r++; // skip the size byte
                // use memcmp as we (will) allow zeros in UTF8 strings
                int res = memcmp(l, r, sz);
                if( res ) 
                    return res;
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
        case cbindata:
            {
                int L = *l;
                int R = *r;
                int diff = L-R; // checks length and subtype simultaneously
                if( diff )
                    return diff;
                // same length, same type
                l++; r++;
                int len = binDataCodeToLength(L);
                int res = memcmp(l, r, len);
                if( res ) 
                    return res;
                l += len; r += len;
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
        BSONObj L = toBson();
        BSONObj R = right.toBson();
        return L.woCompare(R, order, /*considerfieldname*/false);
    }

    int KeyV1::woCompare(const KeyV1& right, const Ordering &order) const {
        const unsigned char *l = _keyData;
        const unsigned char *r = right._keyData;

        if( (*l|*r) == IsBSON ) // only can do this if cNOTUSED maintained
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

        if( (*l|*r) == IsBSON ) {
            return toBson().woEqual(right.toBson());
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
        if( !isCompactFormat() ) {
            return bson().objsize() + 1;
        }

        bool more;
        do { 
            unsigned type = *p & cCANONTYPEMASK;
            unsigned z = sizes[type];
            if( z == 0 ) {
                if( type == cstring ) { 
                    z = ((unsigned) p[1]) + 2;
                }
                else {
                    assert( type == cbindata );
                    z = binDataCodeToLength(p[1]) + 2;
                }
            }
            more = (*p & cHASMORE) != 0;
            p += z;
        } while( more );
        return p - _keyData;
    }

    struct CmpUnitTest : public UnitTest {
        void run() {
            char a[2];
            char b[2];
            a[0] = -3;
            a[1] = 0;
            b[0] = 3;
            b[1] = 0;
            assert( strcmp(a,b)>0 && memcmp(a,b,2)>0 );
        }
    } cunittest;

}
