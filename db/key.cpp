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
        cX = 0x20,
        cHASMORE = 0x40,
        cISKEY = 0x80
    };

    BSONObj KeyV1::toBson() const { 
        if( _keyData == 0 )
            return _o;

        BSONObjBuilder b(512);
        unsigned char *p = _keyData;
        while( 1 ) { 
            unsigned bits = *p++;

            switch( bits & 0x3f ) {
                case cminkey: b.appendMinKey(""); break;
                case cnull:   b.appendNull(""); break;
                case cfalse:  b.appendBool("", false);
                case ctrue:   b.appendBool("", true);
                case cmaxkey: b.appendMaxKey(""); break;
                case cstring:
                    int sz = strlen((const char *) p) + 1;
                    b.append("", (const char *) p, sz);
                    p += sz;
                    break;
                case coid:
                    b.appendOID("", (OID *) p);
                    p += sizeof(OID);
                    break;
                case cdate:
                    b.appendDate("", (Date_t&) *p);
                    p += 8;
                    break;
                case cdouble:

            }

            if( (bits & cHASMORE) == 0 )
                break;
        }
        return b.obj();
    }

    static int compare(unsigned char *&l, unsigned char *&r) { 
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
                    return 1;
                if( L > R )
                    return -1;
                l += 8; r += 8;
                break;
            }
        case cstring:
            {
                int res = strcmp((const char *) l, (const char *) r);
                if( res ) 
                    return res;
                int len = strlen((const char *) l) + 1;
                l += len; r += len;
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
                    return 1;
                if( L > R )
                    return -1;
                l += 8; r += 8;
                break;
            }
        default:
            // all the others are a match -- e.g. null == null
            ;
        }

        return 0;
    }

    int NOINLINE_DECL KeyV1::compareHybrid(const KeyV1& right, const Ordering& order) const { 
        BSONObj L = _keyData == 0 ? _o : toBson();
        BSONObj R = right._keyData == 0 ? right._o : right.toBson();
        return L.woCompare(R, order);
    }

    int KeyV1::woCompare(const KeyV1& right, const Ordering &order) const {
        unsigned char *l = _keyData;
        unsigned char *r = right._keyData;

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

}
