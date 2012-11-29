/**
*    Copyright (C) 2012 10gen Inc.
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

#ifndef GEODEBUG
#define GEODEBUG(x)
#endif

namespace mongo {
    class Point {
    public:
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


    class Box {
    public:
        Box( double x , double y , double size )
            : _min( x , y ) ,
              _max( x + size , y + size ) {
        }

        Box( Point min , Point max )
            : _min( min ) , _max( max ) {
        }

        Box() {}

        BSONArray toBSON() const {
            return BSON_ARRAY( BSON_ARRAY( _min._x << _min._y ) << BSON_ARRAY( _max._x << _max._y ) );
        }

        string toString() const {
            StringBuilder buf;
            buf << _min.toString() << " -->> " << _max.toString();
            return buf.str();
        }

        bool between( double min , double max , double val , double fudge=0) const {
            return val + fudge >= min && val <= max + fudge;
        }

        bool onBoundary( double bound, double val, double fudge = 0 ) const {
            return ( val >= bound - fudge && val <= bound + fudge );
        }

        bool mid( double amin , double amax , double bmin , double bmax , bool min , double& res ) const {
            verify( amin <= amax );
            verify( bmin <= bmax );

            if ( amin < bmin ) {
                if ( amax < bmin )
                    return false;
                res = min ? bmin : amax;
                return true;
            }
            if ( amin > bmax )
                return false;
            res = min ? amin : bmax;
            return true;
        }

        double intersects( const Box& other ) const {

            Point boundMin(0,0);
            Point boundMax(0,0);

            if ( mid( _min._x , _max._x , other._min._x , other._max._x , true , boundMin._x ) == false ||
                    mid( _min._x , _max._x , other._min._x , other._max._x , false , boundMax._x ) == false ||
                    mid( _min._y , _max._y , other._min._y , other._max._y , true , boundMin._y ) == false ||
                    mid( _min._y , _max._y , other._min._y , other._max._y , false , boundMax._y ) == false )
                return 0;

            Box intersection( boundMin , boundMax );

            return intersection.area() / area();
        }

        double area() const {
            return ( _max._x - _min._x ) * ( _max._y - _min._y );
        }

        double maxDim() const {
            return max( _max._x - _min._x, _max._y - _min._y );
        }
        // pointFromHash
        // pointToHash
        // boxFromHash

        Point center() const {
            return Point( ( _min._x + _max._x ) / 2 ,
                          ( _min._y + _max._y ) / 2 );
        }

        void truncate(double min, double max) {
            if( _min._x < min ) _min._x = min;
            if( _min._y < min ) _min._y = min;
            if( _max._x > max ) _max._x = max;
            if( _max._y > max ) _max._y = max;
        }

        void fudge(double error) {
            _min._x -= error;
            _min._y -= error;
            _max._x += error;
            _max._y += error;
        }

        bool onBoundary( Point p, double fudge = 0 ) {
            return onBoundary( _min._x, p._x, fudge ) ||
                   onBoundary( _max._x, p._x, fudge ) ||
                   onBoundary( _min._y, p._y, fudge ) ||
                   onBoundary( _max._y, p._y, fudge );
        }

        bool inside( Point p , double fudge = 0 ) const {
            bool res = inside( p._x , p._y , fudge );
            //cout << "is : " << p.toString() << " in " << toString() << " = " << res << endl;
            return res;
        }

        bool inside( double x , double y , double fudge = 0 ) const {
            return
                between( _min._x , _max._x  , x , fudge ) &&
                between( _min._y , _max._y  , y , fudge );
        }

        bool contains(const Box& other, double fudge=0) {
            return inside(other._min, fudge) && inside(other._max, fudge);
        }

        Point _min;
        Point _max;
    };


    class Polygon {
    public:

        Polygon( void ) : _centroidCalculated( false ) {}

        Polygon( vector<Point> points ) : _centroidCalculated( false ),
            _points( points ) { }

        void add( Point p ) {
            _centroidCalculated = false;
            _points.push_back( p );
        }

        int size( void ) const {
            return _points.size();
        }

        /**
         * Determine if the point supplied is contained by the current polygon.
         *
         * The algorithm uses a ray casting method.
         */
        bool contains( const Point& p ) const {
            return contains( p, 0 ) > 0;
        }

        int contains( const Point &p, double fudge ) const {

            Box fudgeBox( Point( p._x - fudge, p._y - fudge ), Point( p._x + fudge, p._y + fudge ) );

            int counter = 0;
            Point p1 = _points[0];
            for ( int i = 1; i <= size(); i++ ) {
                Point p2 = _points[i % size()];

                GEODEBUG( "Doing intersection check of " << fudgeBox.toString() << " with seg " << p1.toString() << " to " << p2.toString() );

                // We need to check whether or not this segment intersects our error box
                if( fudge > 0 &&
                        // Points not too far below box
                        fudgeBox._min._y <= std::max( p1._y, p2._y ) &&
                        // Points not too far above box
                        fudgeBox._max._y >= std::min( p1._y, p2._y ) &&
                        // Points not too far to left of box
                        fudgeBox._min._x <= std::max( p1._x, p2._x ) &&
                        // Points not too far to right of box
                        fudgeBox._max._x >= std::min( p1._x, p2._x ) ) {

                    GEODEBUG( "Doing detailed check" );

                    // If our box contains one or more of these points, we need to do an exact check.
                    if( fudgeBox.inside(p1) ) {
                        GEODEBUG( "Point 1 inside" );
                        return 0;
                    }
                    if( fudgeBox.inside(p2) ) {
                        GEODEBUG( "Point 2 inside" );
                        return 0;
                    }

                    // Do intersection check for vertical sides
                    if ( p1._y != p2._y ) {

                        double invSlope = ( p2._x - p1._x ) / ( p2._y - p1._y );

                        double xintersT = ( fudgeBox._max._y - p1._y ) * invSlope + p1._x;
                        if( fudgeBox._min._x <= xintersT && fudgeBox._max._x >= xintersT ) {
                            GEODEBUG( "Top intersection @ " << xintersT );
                            return 0;
                        }

                        double xintersB = ( fudgeBox._min._y - p1._y ) * invSlope + p1._x;
                        if( fudgeBox._min._x <= xintersB && fudgeBox._max._x >= xintersB ) {
                            GEODEBUG( "Bottom intersection @ " << xintersB );
                            return 0;
                        }

                    }

                    // Do intersection check for horizontal sides
                    if( p1._x != p2._x ) {

                        double slope = ( p2._y - p1._y ) / ( p2._x - p1._x );

                        double yintersR = ( p1._x - fudgeBox._max._x ) * slope + p1._y;
                        if( fudgeBox._min._y <= yintersR && fudgeBox._max._y >= yintersR ) {
                            GEODEBUG( "Right intersection @ " << yintersR );
                            return 0;
                        }

                        double yintersL = ( p1._x - fudgeBox._min._x ) * slope + p1._y;
                        if( fudgeBox._min._y <= yintersL && fudgeBox._max._y >= yintersL ) {
                            GEODEBUG( "Left intersection @ " << yintersL );
                            return 0;
                        }

                    }

                }
                else if( fudge == 0 ){

                    // If this is an exact vertex, we won't intersect, so check this
                    if( p._y == p1._y && p._x == p1._x ) return 1;
                    else if( p._y == p2._y && p._x == p2._x ) return 1;

                    // If this is a horizontal line we won't intersect, so check this
                    if( p1._y == p2._y && p._y == p1._y ){
                        // Check that the x-coord lies in the line
                        if( p._x >= std::min( p1._x, p2._x ) && p._x <= std::max( p1._x, p2._x ) ) return 1;
                    }

                }

                // Normal intersection test.
                // TODO: Invert these for clearer logic?
                if ( p._y > std::min( p1._y, p2._y ) ) {
                    if ( p._y <= std::max( p1._y, p2._y ) ) {
                        if ( p._x <= std::max( p1._x, p2._x ) ) {
                            if ( p1._y != p2._y ) {
                                double xinters = (p._y-p1._y)*(p2._x-p1._x)/(p2._y-p1._y)+p1._x;
                                // Special case of point on vertical line
                                if ( p1._x == p2._x && p._x == p1._x ){

                                    // Need special case for the vertical edges, for example:
                                    // 1) \e   pe/----->
                                    // vs.
                                    // 2) \ep---e/----->
                                    //
                                    // if we count exact as intersection, then 1 is in but 2 is out
                                    // if we count exact as no-int then 1 is out but 2 is in.

                                    return 1;
                                }
                                else if( p1._x == p2._x || p._x <= xinters ) {
                                    counter++;
                                }
                            }
                        }
                    }
                }

                p1 = p2;
            }

            if ( counter % 2 == 0 ) {
                return -1;
            }
            else {
                return 1;
            }
        }

        /**
         * Calculate the centroid, or center of mass of the polygon object.
         */
        Point centroid( void ) {

            /* Centroid is cached, it won't change betwen points */
            if ( _centroidCalculated ) {
                return _centroid;
            }

            Point cent;
            double signedArea = 0.0;
            double area = 0.0;  // Partial signed area

            /// For all vertices except last
            int i = 0;
            for ( i = 0; i < size() - 1; ++i ) {
                area = _points[i]._x * _points[i+1]._y - _points[i+1]._x * _points[i]._y ;
                signedArea += area;
                cent._x += ( _points[i]._x + _points[i+1]._x ) * area;
                cent._y += ( _points[i]._y + _points[i+1]._y ) * area;
            }

            // Do last vertex
            area = _points[i]._x * _points[0]._y - _points[0]._x * _points[i]._y;
            cent._x += ( _points[i]._x + _points[0]._x ) * area;
            cent._y += ( _points[i]._y + _points[0]._y ) * area;
            signedArea += area;
            signedArea *= 0.5;
            cent._x /= ( 6 * signedArea );
            cent._y /= ( 6 * signedArea );

            _centroidCalculated = true;
            _centroid = cent;

            return cent;
        }

        Box bounds( void ) {

            // TODO: Cache this

            _bounds._max = _points[0];
            _bounds._min = _points[0];

            for ( int i = 1; i < size(); i++ ) {

                _bounds._max._x = max( _bounds._max._x, _points[i]._x );
                _bounds._max._y = max( _bounds._max._y, _points[i]._y );
                _bounds._min._x = min( _bounds._min._x, _points[i]._x );
                _bounds._min._y = min( _bounds._min._y, _points[i]._y );

            }

            return _bounds;

        }

    private:

        bool _centroidCalculated;
        Point _centroid;

        Box _bounds;

        vector<Point> _points;
    };
}  // namespace mongo
