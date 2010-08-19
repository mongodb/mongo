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
        GeoBitSets(){
            for ( int i=0; i<32; i++ ){
                masks32[i] = ( 1 << ( 31 - i ) );
            }
            for ( int i=0; i<64; i++ ){
                masks64[i] = ( 1LL << ( 63 - i ) );
            }
            
            for ( unsigned i=0; i<16; i++ ){
                unsigned fixed = 0;
                for ( int j=0; j<4; j++ ){
                    if ( i & ( 1 << j ) )
                        fixed |= ( 1 << ( j * 2 ) );
                }
                hashedToNormal[fixed] = i;
            }
            
        }
        int masks32[32];
        long long masks64[64];

        unsigned hashedToNormal[256];
    };

    extern GeoBitSets geoBitSets;
    
    class GeoHash {
    public:
        GeoHash()
            : _hash(0),_bits(0){
        }

        explicit GeoHash( const char * hash ){
            init( hash );
        }

        explicit GeoHash( const string& hash ){
            init( hash );
        }

        explicit GeoHash( const BSONElement& e , unsigned bits=32 ){
            _bits = bits;
            if ( e.type() == BinData ){
                int len = 0;
                _copy( (char*)&_hash , e.binData( len ) );
                assert( len == 8 );
                _bits = bits;
            }
            else {
                cout << "GeoHash cons e : " << e << endl;
                uassert(13047,"wrong type for geo index. if you're using a pre-release version, need to rebuild index",0);
            }
            _fix();
        }
        
        GeoHash( unsigned y , unsigned x , unsigned bits=32){
            init( y , x , bits );
        }

        GeoHash( const GeoHash& old ){
            _hash = old._hash;
            _bits = old._bits;
        }

        GeoHash( long long hash , unsigned bits )
            : _hash( hash ) , _bits( bits ){
            _fix();
        }

        void init( unsigned y , unsigned x , unsigned bits ){
            assert( bits <= 32 );
            _hash = 0;
            _bits = bits;
            for ( unsigned i=0; i<bits; i++ ){
                if ( isBitSet( y , i ) ) _hash |= geoBitSets.masks64[i*2];
                if ( isBitSet( x , i ) ) _hash |= geoBitSets.masks64[(i*2)+1];
            }
        }

        void unhash_fast( unsigned& y , unsigned& x ) const {
            y = 0;
            x = 0;
            char * c = (char*)(&_hash);
            for ( int i=0; i<8; i++ ){
                unsigned t = (unsigned)(c[i]) & 0x55;
                x |= ( geoBitSets.hashedToNormal[t] << (4*(i)) );

                t = ( (unsigned)(c[i]) >> 1 ) & 0x55;
                y |= ( geoBitSets.hashedToNormal[t] << (4*(i)) );
            }
        }

        void unhash_slow( unsigned& y , unsigned& x ) const {
            y = 0;
            x = 0;
            for ( unsigned i=0; i<_bits; i++ ){
                if ( getGitY(i) )
                    y |= geoBitSets.masks32[i];
                if ( getBitX(i) )
                    x |= geoBitSets.masks32[i];
            }
        }

        void unhash( unsigned& y , unsigned& x ) const {
            unhash_fast( y , x );
        }

        /**
         * @param 0 = high
         */
        static bool isBitSet( unsigned val , unsigned  bit ){
            return geoBitSets.masks32[bit] & val;
        }
        
        GeoHash up() const {
            return GeoHash( _hash , _bits - 1 );
        }
        
        bool hasPrefix( const GeoHash& other ) const {
            assert( other._bits <= _bits );
            if ( other._bits == 0 )
                return true;
            long long y = other._hash ^ _hash;
            y = y >> (64-(other._bits*2));
            return y == 0;
        }
        

        string toString() const { 
            StringBuilder buf( _bits * 2 );
            for ( unsigned y=0; y<_bits*2; y++ )
                buf.append( _hash & geoBitSets.masks64[y] ? "1" : "0" );
            return buf.str();
        }

        string toStringHex1() const {
            stringstream ss;
            ss << hex << _hash;
            return ss.str();
        }

        void init( const string& s ){
            _hash = 0;
            _bits = s.size() / 2;
            for ( unsigned pos=0; pos<s.size(); pos++ )
                if ( s[pos] == '1' )
                    setBit( pos , 1 );
        }

        void setBit( unsigned pos , bool one ){
            assert( pos < _bits * 2 );
            if ( one )
                _hash |= geoBitSets.masks64[pos];
            else if ( _hash & geoBitSets.masks64[pos] )
                _hash &= ~geoBitSets.masks64[pos];
        }
        
        bool getBit( unsigned pos ) const {
            return _hash & geoBitSets.masks64[pos];
        }

        bool getGitY( unsigned pos ) const {
            assert( pos < 32 );
            return getBit( pos * 2 );
        }

        bool getBitX( unsigned pos ) const {
            assert( pos < 32 );
            return getBit( ( pos * 2 ) + 1 );
        }
        
        BSONObj wrap() const {
            BSONObjBuilder b(20);
            append( b , "" );
            BSONObj o = b.obj();
            assert( o.objsize() == 20 );
            return o;
        }

        bool constrains() const {
            return _bits > 0;
        }
        
        void move( int y , int x ){
            assert( _bits );
            _move( 0 , y );
            _move( 1 , x );
        }

        void _move( unsigned offset , int d ){
            if ( d == 0 )
                return;
            assert( d <= 1 && d>= -1 ); // TEMP
            
            bool from, to;
            if ( d > 0 ){
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
            while ( true ){
                if ( getBit(pos) == from ){
                    setBit( pos , to );
                    return;
                }

                if ( pos < 2 ){
                    // overflow
                    for ( ; pos < ( _bits * 2 ) ; pos += 2 ){
                        setBit( pos , from );
                    }
                    return;
                }
                
                setBit( pos , from );
                pos -= 2;
            }
            
            assert(0);
        }

        GeoHash& operator=(const GeoHash& h) { 
            _hash = h._hash;
            _bits = h._bits;
            return *this;
        }
        
        bool operator==(const GeoHash& h ){
            return _hash == h._hash && _bits == h._bits;
        }

        GeoHash& operator+=( const char * s ) {
            unsigned pos = _bits * 2;
            _bits += strlen(s) / 2;
            assert( _bits <= 32 );
            while ( s[0] ){
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
      
        void _fix(){
            static long long FULL = 0xFFFFFFFFFFFFFFFFLL;
            long long mask = FULL << ( 64 - ( _bits * 2 ) );
            _hash &= mask;
        }
        
        void append( BSONObjBuilder& b , const char * name ) const {
            char buf[8];
            _copy( buf , (char*)&_hash );
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
            for ( ; i<_bits && i<other._bits; i++ ){
                if ( getGitY( i ) == other.getGitY( i ) &&
                     getBitX( i ) == other.getBitX( i ) )
                    continue;
                break;
            }
            return GeoHash(_hash,i);
        }

    private:

        void _copy( char * dst , const char * src ) const {
            for ( unsigned a=0; a<8; a++ ){
                dst[a] = src[7-a];
            }
        }

        long long _hash;
        unsigned _bits; // bits per field, so 1 to 32
    };

    inline ostream& operator<<( ostream &s, const GeoHash &h ){
        s << h.toString();
        return s;
    } 

    class GeoConvert {
    public:
        virtual ~GeoConvert(){}

        virtual void unhash( const GeoHash& h , double& y , double& x ) const = 0;
        virtual GeoHash hash( double y , double x ) const = 0;
    };

    class Point {
    public:
        
        Point( const GeoConvert * g , const GeoHash& hash ){
            g->unhash( hash , _y , _x );
        }
        
        explicit Point( const BSONElement& e ){
            BSONObjIterator i(e.Obj());
            _y = i.next().number();
            _x = i.next().number();
        }

        explicit Point( const BSONObj& o ){
            BSONObjIterator i(o);
            _y = i.next().number();
            _x = i.next().number();
        }

        Point( double y , double x )
            : _y( y ) , _x( x ){
        }
        
        Point() : _y(0),_x(0){
        }

        GeoHash hash( const GeoConvert * g ){
            return g->hash( _y , _x );
        }

        double distance( const Point& p ) const {
            double a = _y - p._y;
            double b = _x - p._x;
            return sqrt( ( a * a ) + ( b * b ) );
        }
        
        string toString() const {
            StringBuilder buf(32);
            buf << "(" << _y << "," << _x << ")";
            return buf.str();
  
        }

        double _y;
        double _x;
    };


    extern const double EARTH_RADIUS_KM;
    extern const double EARTH_RADIUS_MILES;

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

        return acos(cross_prod);
    }

    // note: return is still in radians as that can be multiplied by radius to get arc length
    inline double spheredist_deg( const Point& p1, const Point& p2 ) {
        return spheredist_rad(
                    Point( p1._y * (M_PI/180), p1._x * (M_PI/180)),
                    Point( p2._y * (M_PI/180), p2._x * (M_PI/180))
               );
    }

}
