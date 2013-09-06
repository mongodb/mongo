/**
*    Copyright (C) 2008-2012 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/geo/core.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/util/mongoutils/str.h"

// So we can get at the str namespace.
using namespace mongoutils;

namespace mongo {

////////////// Point

    Point::Point() : x(0), y(0) { }

    Point::Point(double x, double y) : x(x), y(y) { }

    Point::Point(const BSONElement& e) {
        BSONObjIterator i(e.Obj());
        x = i.next().number();
        y = i.next().number();
    }

    Point::Point(const BSONObj& o) {
        BSONObjIterator i(o);
        x = i.next().number();
        y = i.next().number();
    }

    string Point::toString() const {
        StringBuilder buf;
        buf << "(" << x << "," << y << ")";
        return buf.str();
    }

////////////// Circle

    Circle::Circle() {}
    Circle::Circle(double radius, Point center) : radius(radius), center(center) {}


////////////// Box

    Box::Box() {}

    Box::Box(double x, double y, double size) : _min(x, y), _max(x + size, y + size) { }

    Box::Box(Point min, Point max) : _min(min), _max(max) { }

    BSONArray Box::toBSON() const {
        return BSON_ARRAY(BSON_ARRAY(_min.x << _min.y) << BSON_ARRAY(_max.x << _max.y));
    }

    string Box::toString() const {
        StringBuilder buf;
        buf << _min.toString() << " -->> " << _max.toString();
        return buf.str();
    }

    bool Box::between(double min, double max, double val, double fudge) const {
        return val + fudge >= min && val <= max + fudge;
    }

    bool Box::onBoundary(double bound, double val, double fudge) const {
        return (val >= bound - fudge && val <= bound + fudge);
    }

    bool Box::mid(double amin, double amax,
                  double bmin, double bmax, bool min, double* res) const {
        verify(amin <= amax);
        verify(bmin <= bmax);

        if (amin < bmin) {
            if (amax < bmin)
                return false;
            *res = min ? bmin : amax;
            return true;
        }
        if (amin > bmax)
            return false;
        *res = min ? amin : bmax;
        return true;
    }

    double Box::intersects(const Box& other) const {
        Point boundMin(0,0);
        Point boundMax(0,0);

        if (!mid(_min.x, _max.x, other._min.x, other._max.x, true, &boundMin.x) ||
            !mid(_min.x, _max.x, other._min.x, other._max.x, false, &boundMax.x) ||
            !mid(_min.y, _max.y, other._min.y, other._max.y, true, &boundMin.y) ||
            !mid(_min.y, _max.y, other._min.y, other._max.y, false, &boundMax.y))
            return 0;

        Box intersection(boundMin, boundMax);
        return intersection.area() / area();
    }

    double Box::area() const {
        return (_max.x - _min.x) * (_max.y - _min.y);
    }

    double Box::maxDim() const {
        return max(_max.x - _min.x, _max.y - _min.y);
    }

    Point Box::center() const {
        return Point((_min.x + _max.x) / 2,
                     (_min.y + _max.y) / 2);
    }

    void Box::truncate(double min, double max) {
        if (_min.x < min) _min.x = min;
        if (_min.y < min) _min.y = min;
        if (_max.x > max) _max.x = max;
        if (_max.y > max) _max.y = max;
    }

    void Box::fudge(double error) {
        _min.x -= error;
        _min.y -= error;
        _max.x += error;
        _max.y += error;
    }

    bool Box::onBoundary(Point p, double fudge) {
        return onBoundary(_min.x, p.x, fudge) ||
               onBoundary(_max.x, p.x, fudge) ||
               onBoundary(_min.y, p.y, fudge) ||
               onBoundary(_max.y, p.y, fudge);
    }

    bool Box::inside(Point p, double fudge) const {
        bool res = inside(p.x, p.y, fudge);
        return res;
    }

    bool Box::inside(double x, double y, double fudge) const {
        return between(_min.x, _max.x , x, fudge) &&
               between(_min.y, _max.y , y, fudge);
    }

    bool Box::contains(const Box& other, double fudge) {
        return inside(other._min, fudge) && inside(other._max, fudge);
    }

////////////// Polygon

    Polygon::Polygon(void) : _centroidCalculated(false), _boundsCalculated(false) {}

    Polygon::Polygon(vector<Point> points) : _centroidCalculated(false),
                                             _boundsCalculated(false), _points(points) { }

    void Polygon::add(Point p) {
        _centroidCalculated = false;
        _boundsCalculated = false;
        _points.push_back(p);
    }

    int Polygon::size(void) const { return _points.size(); }

    bool Polygon::contains(const Point& p) const { return contains(p, 0) > 0; }

    /* 
     * Return values:
     * -1 if no intersection
     * 0 if maybe an intersection (using fudge)
     * 1 if there is an intersection
     *
     * A ray casting intersection method is used.
     */
    int Polygon::contains(const Point &p, double fudge) const {
        Box fudgeBox(Point(p.x - fudge, p.y - fudge), Point(p.x + fudge, p.y + fudge));

        int counter = 0;
        Point p1 = _points[0];
        for (int i = 1; i <= size(); i++) {
            // XXX: why is there a mod here?
            Point p2 = _points[i % size()];

            GEODEBUG("Doing intersection check of " << fudgeBox.toString()
                     << " with seg " << p1.toString() << " to " << p2.toString());

            // We need to check whether or not this segment intersects our error box
            if (fudge > 0 &&
                    // Points not too far below box
                    fudgeBox._min.y <= std::max(p1.y, p2.y) &&
                    // Points not too far above box
                    fudgeBox._max.y >= std::min(p1.y, p2.y) &&
                    // Points not too far to left of box
                    fudgeBox._min.x <= std::max(p1.x, p2.x) &&
                    // Points not too far to right of box
                    fudgeBox._max.x >= std::min(p1.x, p2.x)) {

                GEODEBUG("Doing detailed check");

                // If our box contains one or more of these points, we need to do an exact
                // check.
                if (fudgeBox.inside(p1)) {
                    GEODEBUG("Point 1 inside");
                    return 0;
                }
                if (fudgeBox.inside(p2)) {
                    GEODEBUG("Point 2 inside");
                    return 0;
                }

                // Do intersection check for vertical sides
                if (p1.y != p2.y) {
                    double invSlope = (p2.x - p1.x) / (p2.y - p1.y);

                    double xintersT = (fudgeBox._max.y - p1.y) * invSlope + p1.x;
                    if (fudgeBox._min.x <= xintersT && fudgeBox._max.x >= xintersT) {
                        GEODEBUG("Top intersection @ " << xintersT);
                        return 0;
                    }

                    double xintersB = (fudgeBox._min.y - p1.y) * invSlope + p1.x;
                    if (fudgeBox._min.x <= xintersB && fudgeBox._max.x >= xintersB) {
                        GEODEBUG("Bottom intersection @ " << xintersB);
                        return 0;
                    }
                }

                // Do intersection check for horizontal sides
                if (p1.x != p2.x) {
                    double slope = (p2.y - p1.y) / (p2.x - p1.x);

                    double yintersR = (p1.x - fudgeBox._max.x) * slope + p1.y;
                    if (fudgeBox._min.y <= yintersR && fudgeBox._max.y >= yintersR) {
                        GEODEBUG("Right intersection @ " << yintersR);
                        return 0;
                    }

                    double yintersL = (p1.x - fudgeBox._min.x) * slope + p1.y;
                    if (fudgeBox._min.y <= yintersL && fudgeBox._max.y >= yintersL) {
                        GEODEBUG("Left intersection @ " << yintersL);
                        return 0;
                    }
                }
            } else if (fudge == 0){
                // If this is an exact vertex, we won't intersect, so check this
                if (p.y == p1.y && p.x == p1.x) return 1;
                else if (p.y == p2.y && p.x == p2.x) return 1;

                // If this is a horizontal line we won't intersect, so check this
                if (p1.y == p2.y && p.y == p1.y){
                    // Check that the x-coord lies in the line
                    if (p.x >= std::min(p1.x, p2.x) && p.x <= std::max(p1.x, p2.x))
                        return 1;
                }
            }

            // Normal intersection test.
            // TODO: Invert these for clearer logic?
            if (p.y > std::min(p1.y, p2.y)) {
                if (p.y <= std::max(p1.y, p2.y)) {
                    if (p.x <= std::max(p1.x, p2.x)) {
                        if (p1.y != p2.y) {
                            double xinters = (p.y-p1.y)*(p2.x-p1.x)/(p2.y-p1.y)+p1.x;
                            // Special case of point on vertical line
                            if (p1.x == p2.x && p.x == p1.x){

                                // Need special case for the vertical edges, for example:
                                // 1) \e   pe/----->
                                // vs.
                                // 2) \ep---e/----->
                                //
                                // if we count exact as intersection, then 1 is in but 2 is out
                                // if we count exact as no-int then 1 is out but 2 is in.

                                return 1;
                            } else if (p1.x == p2.x || p.x <= xinters) {
                                counter++;
                            }
                        }
                    }
                }
            }

            p1 = p2;
        }

        if (counter % 2 == 0) {
            return -1;
        } else {
            return 1;
        }
    }

    Point Polygon::centroid(void) {
        /* Centroid is cached, it won't change betwen points */
        if (_centroidCalculated) {
            return _centroid;
        }

        Point cent;
        double signedArea = 0.0;
        double area = 0.0;  // Partial signed area

        /// For all vertices except last
        int i = 0;
        for (i = 0; i < size() - 1; ++i) {
            area = _points[i].x * _points[i+1].y - _points[i+1].x * _points[i].y ;
            signedArea += area;
            cent.x += (_points[i].x + _points[i+1].x) * area;
            cent.y += (_points[i].y + _points[i+1].y) * area;
        }

        // Do last vertex
        area = _points[i].x * _points[0].y - _points[0].x * _points[i].y;
        cent.x += (_points[i].x + _points[0].x) * area;
        cent.y += (_points[i].y + _points[0].y) * area;
        signedArea += area;
        signedArea *= 0.5;
        cent.x /= (6 * signedArea);
        cent.y /= (6 * signedArea);

        _centroidCalculated = true;
        _centroid = cent;

        return cent;
    }

    Box Polygon::bounds(void) {
        if (_boundsCalculated) {
            return _bounds;
        }

        _bounds._max = _points[0];
        _bounds._min = _points[0];

        for (int i = 1; i < size(); i++) {
            _bounds._max.x = max(_bounds._max.x, _points[i].x);
            _bounds._max.y = max(_bounds._max.y, _points[i].y);
            _bounds._min.x = min(_bounds._min.x, _points[i].x);
            _bounds._min.y = min(_bounds._min.y, _points[i].y);

        }

        _boundsCalculated = true;
        return _bounds;
    }

    /////// Other methods

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
     * (radius + center.x, center.y) or vice-versa.
     */
    bool distanceWithin(const Point &p1, const Point &p2, double radius) {
        double a = p2.x - p1.x;
        double b = p2.y - p1.y;

        if (a == 0) {
            //
            // Note:  For some, unknown reason, when a 32-bit g++ optimizes this call, the sum is
            // calculated imprecisely.  We need to force the compiler to always evaluate it
            // correctly, hence the weirdness.
            //
            // On some 32-bit linux machines, removing the volatile keyword or calculating the sum
            // inline will make certain geo tests fail.  Of course this check will force volatile
            // for all 32-bit systems, not just affected systems.
            if (sizeof(void*) <= 4){
                volatile double sum = p2.y > p1.y ? p1.y + radius : p2.y + radius;
                return p2.y > p1.y ? sum >= p2.y : sum >= p1.y;
            } else {
                // Original math, correct for most systems
                return p2.y > p1.y ? p1.y + radius >= p2.y : p2.y + radius >= p1.y;
            }
        }

        if (b == 0) {
            if (sizeof(void*) <= 4){
                volatile double sum = p2.x > p1.x ? p1.x + radius : p2.x + radius;
                return p2.x > p1.x ? sum >= p2.x : sum >= p1.x;
            } else {
                return p2.x > p1.x ? p1.x + radius >= p2.x : p2.x + radius >= p1.x;
            }
        }

        return sqrt((a * a) + (b * b)) <= radius;
    }

    // Technically lat/long bounds, not really tied to earth radius.
    void checkEarthBounds(const Point &p) {
        uassert(14808, str::stream() << "point " << p.toString()
                                     << " must be in earth-like bounds of long "
                                     << ": [-180, 180], lat : [-90, 90] ",
                p.x >= -180 && p.x <= 180 && p.y >= -90 && p.y <= 90);
    }


    // WARNING: x and y MUST be longitude and latitude in that order
    // note: multiply by earth radius for distance
    double spheredist_rad(const Point& p1, const Point& p2) {
        // this uses the n-vector formula: http://en.wikipedia.org/wiki/N-vector
        // If you try to match the code to the formula, note that I inline the cross-product.

        double sinx1(sin(p1.x)), cosx1(cos(p1.x));
        double siny1(sin(p1.y)), cosy1(cos(p1.y));
        double sinx2(sin(p2.x)), cosx2(cos(p2.x));
        double siny2(sin(p2.y)), cosy2(cos(p2.y));

        double cross_prod =
            (cosy1*cosx1 * cosy2*cosx2) +
            (cosy1*sinx1 * cosy2*sinx2) +
            (siny1        * siny2);

        if (cross_prod >= 1 || cross_prod <= -1) {
            // fun with floats
            verify(fabs(cross_prod)-1 < 1e-6);
            return cross_prod > 0 ? 0 : M_PI;
        }

        return acos(cross_prod);
    }

    // @param p1 A point on the sphere where x and y are degrees.
    // @param p2 A point on the sphere where x and y are degrees.
    // @return The distance between the two points in RADIANS.  Multiply by radius to get arc
    // length.
    double spheredist_deg(const Point& p1, const Point& p2) {
        return spheredist_rad(Point(deg2rad(p1.x), deg2rad(p1.y)),
                              Point(deg2rad(p2.x), deg2rad(p2.y)));
    }

    double distance(const Point& p1, const Point &p2) {
        double a = p1.x - p2.x;
        double b = p1.y - p2.y;

        // Avoid numerical error if possible...
        if (a == 0) return abs(b);
        if (b == 0) return abs(a);

        return sqrt((a * a) + (b * b));
    }

}  // namespace mongo
