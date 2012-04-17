// core.h

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

#include "../../pch.h"
#include "../jsobj.h"

#include <cmath>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace mongo {

    class GeoBitSets {
    public:
        GeoBitSets() {
            for ( int i=0; i<32; i++ ) {
                masks32[i] = ( 1 << ( 31 - i ) );
            }
            for ( int i=0; i<64; i++ ) {
                masks64[i] = ( 1LL << ( 63 - i ) );
            }

            for ( unsigned i=0; i<16; i++ ) {
                unsigned fixed = 0;
                for ( int j=0; j<4; j++ ) {
                    if ( i & ( 1 << j ) )
                        fixed |= ( 1 << ( j * 2 ) );
                }
                hashedToNormal[fixed] = i;
            }

            long long currAllX = 0, currAllY = 0;
            for ( int i = 0; i < 64; i++ ){
                if( i % 2 == 0 ){
                    allX[ i / 2 ] = currAllX;
                    currAllX = currAllX + ( 1LL << ( 63 - i ) );
                }
                else{
                    allY[ i / 2 ] = currAllY;
                    currAllY = currAllY + ( 1LL << ( 63 - i ) );
                }
            }
        }
        int masks32[32];
        long long masks64[64];
        long long allX[32];
        long long allY[32];

        unsigned hashedToNormal[256];
    };

    extern GeoBitSets geoBitSets;

    class GeoHash {
    public:

        GeoHash()
            : _hash(0),_bits(0) {
        }

        explicit GeoHash( const char * hash ) {
            init( hash );
        }

        explicit GeoHash( const string& hash ) {
            init( hash );
        }

        static GeoHash makeFromBinData(const char *bindata, unsigned bits) {
            GeoHash h;
            h._bits = bits;
            h._hash = big<long long>::ref( bindata );
            h._fix();
            return h;
        }

        explicit GeoHash( const BSONElement& e , unsigned bits=32 ) {
            _bits = bits;
            if ( e.type() == BinData ) {
                int len = 0;
                _hash = big<long long>::ref( e.binData( len ) );
                verify( len == 8 );
                _bits = bits;
            }
            else {
                cout << "GeoHash bad element: " << e << endl;
                uassert(13047,"wrong type for geo index. if you're using a pre-release version, need to rebuild index",0);
            }
            _fix();
        }

        GeoHash( unsigned x , unsigned y , unsigned bits=32) {
            init( x , y , bits );
        }

        GeoHash( const GeoHash& old ) {
            _hash = old._hash;
            _bits = old._bits;
        }

        GeoHash( long long hash , unsigned bits )
            : _hash( hash ) , _bits( bits ) {
            _fix();
        }

        void init( unsigned x , unsigned y , unsigned bits ) {
            verify( bits <= 32 );
            _hash = 0;
            _bits = bits;
            for ( unsigned i=0; i<bits; i++ ) {
                if ( isBitSet( x , i ) ) _hash |= geoBitSets.masks64[i*2];
                if ( isBitSet( y , i ) ) _hash |= geoBitSets.masks64[(i*2)+1];
            }
        }

        void unhash_fast( unsigned& x , unsigned& y ) const {
            x = 0;
            y = 0;
            for ( int i=0; i<8; i++ ) {
                unsigned char c_i = _hash >> ( i * 8 );
                unsigned t = c_i & 0x55;
                y |= ( geoBitSets.hashedToNormal[t] << (4*(i)) );
                
                t = ( c_i >> 1 ) & 0x55;
                x |= ( geoBitSets.hashedToNormal[t] << (4*(i)) );
            }

        }

        void unhash_slow( unsigned& x , unsigned& y ) const {
            x = 0;
            y = 0;
            for ( unsigned i=0; i<_bits; i++ ) {
                if ( getBitX(i) )
                    x |= geoBitSets.masks32[i];
                if ( getBitY(i) )
                    y |= geoBitSets.masks32[i];
            }
        }

        void unhash( unsigned& x , unsigned& y ) const {
            unhash_fast( x , y );
        }

        /**
         * @param 0 = high
         */
        static bool isBitSet( unsigned val , unsigned  bit ) {
            return geoBitSets.masks32[bit] & val;
        }

        GeoHash up() const {
            return GeoHash( _hash , _bits - 1 );
        }

        bool hasPrefix( const GeoHash& other ) const {
            verify( other._bits <= _bits );
            if ( other._bits == 0 )
                return true;
            long long x = other._hash ^ _hash;
            x = x >> (64-(other._bits*2));
            return x == 0;
        }


        string toString() const {
            StringBuilder buf;
            for ( unsigned x=0; x<_bits*2; x++ )
                buf.append( _hash & geoBitSets.masks64[x] ? "1" : "0" );
            return buf.str();
        }

        string toStringHex1() const {
            stringstream ss;
            ss << hex << _hash;
            return ss.str();
        }

        void init( const string& s ) {
            _hash = 0;
            _bits = s.size() / 2;
            for ( unsigned pos=0; pos<s.size(); pos++ )
                if ( s[pos] == '1' )
                    setBit( pos , 1 );
        }

        void setBit( unsigned pos , bool one ) {
            verify( pos < _bits * 2 );
            if ( one )
                _hash |= geoBitSets.masks64[pos];
            else if ( _hash & geoBitSets.masks64[pos] )
                _hash &= ~geoBitSets.masks64[pos];
        }

        bool getBit( unsigned pos ) const {
            return _hash & geoBitSets.masks64[pos];
        }

        bool getBitX( unsigned pos ) const {
            verify( pos < 32 );
            return getBit( pos * 2 );
        }

        bool getBitY( unsigned pos ) const {
            verify( pos < 32 );
            return getBit( ( pos * 2 ) + 1 );
        }

        BSONObj wrap( const char* name = "" ) const {
            BSONObjBuilder b(20);
            append( b , name );
            BSONObj o = b.obj();
            if( ! strlen( name ) ) verify( o.objsize() == 20 );
            return o;
        }

        bool constrains() const {
            return _bits > 0;
        }

        bool canRefine() const {
           return _bits < 32;
        }

        bool atMinX() const {
            return ( _hash & geoBitSets.allX[ _bits ] ) == 0;
        }

        bool atMinY() const {
            //log() << " MinY : " << hex << (unsigned long long) _hash << " " << _bits << " " << hex << (unsigned long long) geoBitSets.allY[ _bits ] << endl;
            return ( _hash & geoBitSets.allY[ _bits ] ) == 0;
        }

        bool atMaxX() const {
            return ( _hash & geoBitSets.allX[ _bits ] ) == geoBitSets.allX[ _bits ];
        }

        bool atMaxY() const {
            return ( _hash & geoBitSets.allY[ _bits ] ) == geoBitSets.allY[ _bits ];
        }

        void move( int x , int y ) {
            verify( _bits );
            _move( 0 , x );
            _move( 1 , y );
        }

        void _move( unsigned offset , int d ) {
            if ( d == 0 )
                return;
            verify( d <= 1 && d>= -1 ); // TEMP

            bool from, to;
            if ( d > 0 ) {
                from = 0;
                to = 1;
            }
            else {
                from = 1;
                to = 0;
            }

            unsigned pos = ( _bits * 2 ) - 1;
            if ( offset == 0 )
                pos--;
            while ( true ) {
                if ( getBit(pos) == from ) {
                    setBit( pos , to );
                    return;
                }

                if ( pos < 2 ) {
                    // overflow
                    for ( ; pos < ( _bits * 2 ) ; pos += 2 ) {
                        setBit( pos , from );
                    }
                    return;
                }

                setBit( pos , from );
                pos -= 2;
            }

            verify(0);
        }

        GeoHash& operator=(const GeoHash& h) {
            _hash = h._hash;
            _bits = h._bits;
            return *this;
        }

        bool operator==(const GeoHash& h ) const {
            return _hash == h._hash && _bits == h._bits;
        }

        bool operator!=(const GeoHash& h ) const {
            return !( *this == h );
        }

        bool operator<(const GeoHash& h ) const {
            if( _hash != h._hash ) return _hash < h._hash;
            return _bits < h._bits;
        }

        GeoHash& operator+=( const char * s ) {
            unsigned pos = _bits * 2;
            _bits += strlen(s) / 2;
            verify( _bits <= 32 );
            while ( s[0] ) {
                if ( s[0] == '1' )
                    setBit( pos , 1 );
                pos++;
                s++;
            }

            return *this;
        }

        GeoHash operator+( const char * s ) const {
            GeoHash n = *this;
            n+=s;
            return n;
        }

        GeoHash operator+( string s ) const {
           return operator+( s.c_str() );
        }

        void _fix() {
            static long long FULL = 0xFFFFFFFFFFFFFFFFLL;
            long long mask = FULL << ( 64 - ( _bits * 2 ) );
            _hash &= mask;
        }

        void append( BSONObjBuilder& b , const char * name ) const {
            char buf[8];
            big<long long>::ref( buf ) = _hash;
            b.appendBinData( name , 8 , bdtCustom , buf );
        }

        long long getHash() const {
            return _hash;
        }

        unsigned getBits() const {
            return _bits;
        }

        GeoHash commonPrefix( const GeoHash& other ) const {
            unsigned i=0;
            for ( ; i<_bits && i<other._bits; i++ ) {
                if ( getBitX( i ) == other.getBitX( i ) &&
                        getBitY( i ) == other.getBitY( i ) )
                    continue;
                break;
            }
            return GeoHash(_hash,i);
        }

    private:
        long long _hash;
        unsigned _bits; // bits per field, so 1 to 32
    };

    inline ostream& operator<<( ostream &s, const GeoHash &h ) {
        s << h.toString();
        return s;
    }

    class GeoConvert {
    public:
        virtual ~GeoConvert() {}

        virtual void unhash( const GeoHash& h , double& x , double& y ) const = 0;
        virtual GeoHash hash( double x , double y ) const = 0;
    };

    class Point {
    public:

        Point( const GeoConvert * g , const GeoHash& hash ) {
            g->unhash( hash , _x , _y );
        }

        explicit Point( const BSONElement& e ) {
            BSONObjIterator i(e.Obj());
            _x = i.next().number();
            _y = i.next().number();
        }

        explicit Point( const BSONObj& o ) {
            BSONObjIterator i(o);
            _x = i.next().number();
            _y = i.next().number();
        }

        Point( double x , double y )
            : _x( x ) , _y( y ) {
        }

        Point() : _x(0),_y(0) {
        }

        GeoHash hash( const GeoConvert * g ) {
            return g->hash( _x , _y );
        }

        double distance( const Point& p ) const {
            double a = _x - p._x;
            double b = _y - p._y;

            // Avoid numerical error if possible...
            if( a == 0 ) return abs( _y - p._y );
            if( b == 0 ) return abs( _x - p._x );

            return sqrt( ( a * a ) + ( b * b ) );
        }

        /**
         * Distance method that compares x or y coords when other direction is zero,
         * avoids numerical error when distances are very close to radius but axis-aligned.
         *
         * An example of the problem is:
         * (52.0 - 51.9999) - 0.0001 = 3.31965e-15 and 52.0 - 51.9999 > 0.0001 in double arithmetic
         * but:
         * 51.9999 + 0.0001 <= 52.0
         *
         * This avoids some (but not all!) suprising results in $center queries where points are
         * ( radius + center.x, center.y ) or vice-versa.
         */
        bool distanceWithin( const Point& p, double radius ) const {
            double a = _x - p._x;
            double b = _y - p._y;

            if( a == 0 ) {
                //
                // Note:  For some, unknown reason, when a 32-bit g++ optimizes this call, the sum is
                // calculated imprecisely.  We need to force the compiler to always evaluate it correctly,
                // hence the weirdness.
                //
                // On some 32-bit linux machines, removing the volatile keyword or calculating the sum inline
                // will make certain geo tests fail.  Of course this check will force volatile for all 32-bit systems,
                // not just affected systems.
                if( sizeof(void*) <= 4 ){
                    volatile double sum = _y > p._y ? p._y + radius : _y + radius;
                    return _y > p._y ? sum >= _y : sum >= p._y;
                }
                else {
                    // Original math, correct for most systems
                    return _y > p._y ? p._y + radius >= _y : _y + radius >= p._y;
                }
            }
            if( b == 0 ) {
                if( sizeof(void*) <= 4 ){
                    volatile double sum = _x > p._x ? p._x + radius : _x + radius;
                    return _x > p._x ? sum >= _x : sum >= p._x;
                }
                else {
                    return _x > p._x ? p._x + radius >= _x : _x + radius >= p._x;
                }
            }

            return sqrt( ( a * a ) + ( b * b ) ) <= radius;
        }

        string toString() const {
            StringBuilder buf;
            buf << "(" << _x << "," << _y << ")";
            return buf.str();

        }

        double _x;
        double _y;
    };


    extern const double EARTH_RADIUS_KM;
    extern const double EARTH_RADIUS_MILES;

    // Technically lat/long bounds, not really tied to earth radius.
    inline void checkEarthBounds( Point p ) {
        uassert( 14808, str::stream() << "point " << p.toString() << " must be in earth-like bounds of long : [-180, 180), lat : [-90, 90] ",
                 p._x >= -180 && p._x < 180 && p._y >= -90 && p._y <= 90 );
    }

    inline double deg2rad(double deg) { return deg * (M_PI/180); }
    inline double rad2deg(double rad) { return rad * (180/M_PI); }

    // WARNING: _x and _y MUST be longitude and latitude in that order
    // note: multiply by earth radius for distance
    inline double spheredist_rad( const Point& p1, const Point& p2 ) {
        // this uses the n-vector formula: http://en.wikipedia.org/wiki/N-vector
        // If you try to match the code to the formula, note that I inline the cross-product.
        // TODO: optimize with SSE

        double sin_x1(sin(p1._x)), cos_x1(cos(p1._x));
        double sin_y1(sin(p1._y)), cos_y1(cos(p1._y));
        double sin_x2(sin(p2._x)), cos_x2(cos(p2._x));
        double sin_y2(sin(p2._y)), cos_y2(cos(p2._y));

        double cross_prod =
            (cos_y1*cos_x1 * cos_y2*cos_x2) +
            (cos_y1*sin_x1 * cos_y2*sin_x2) +
            (sin_y1        * sin_y2);

        if (cross_prod >= 1 || cross_prod <= -1) {
            // fun with floats
            verify( fabs(cross_prod)-1 < 1e-6 );
            return cross_prod > 0 ? 0 : M_PI;
        }

        return acos(cross_prod);
    }

    // note: return is still in radians as that can be multiplied by radius to get arc length
    inline double spheredist_deg( const Point& p1, const Point& p2 ) {
        return spheredist_rad(
                   Point( deg2rad(p1._x), deg2rad(p1._y) ),
                   Point( deg2rad(p2._x), deg2rad(p2._y) )
               );
    }

}
