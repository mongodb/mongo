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

#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/geo/big_polygon.h"
#include "mongo/db/geo/s2.h"
#include "mongo/db/jsobj.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2latlng.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polyline.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace mongo {

struct Point;
struct Circle;
class Box;
class Polygon;

inline double deg2rad(const double deg) {
    return deg * (M_PI / 180.0);
}

inline double rad2deg(const double rad) {
    return rad * (180.0 / M_PI);
}

inline double computeXScanDistance(double y, double maxDistDegrees) {
    // TODO: this overestimates for large maxDistDegrees far from the equator
    return maxDistDegrees / std::min(cos(deg2rad(std::min(+89.0, y + maxDistDegrees))),
                                     cos(deg2rad(std::max(-89.0, y - maxDistDegrees))));
}

bool isValidLngLat(double lng, double lat);
bool linesIntersect(const Point& pA, const Point& pB, const Point& pC, const Point& pD);
bool circleContainsBox(const Circle& circle, const Box& box);
bool circleInteriorContainsBox(const Circle& circle, const Box& box);
bool circleIntersectsWithBox(const Circle& circle, const Box& box);
bool circleInteriorIntersectsWithBox(const Circle& circle, const Box& box);
bool edgesIntersectsWithBox(const std::vector<Point>& vertices, const Box& box);
bool polygonContainsBox(const Polygon& polygon, const Box& box);
bool polygonIntersectsWithBox(const Polygon& polygon, const Box& box);

/**
 * Distance utilities for R2 geometries
 */
double distance(const Point& p1, const Point& p2);
bool distanceWithin(const Point& p1, const Point& p2, double radius);
double distanceCompare(const Point& p1, const Point& p2, double radius);
// Still needed for non-wrapping $nearSphere
double spheredist_rad(const Point& p1, const Point& p2);
double spheredist_deg(const Point& p1, const Point& p2);


/**
 * Distance utilities for S2 geometries
 */
struct S2Distance {
    static double distanceRad(const S2Point& pointA, const S2Point& pointB);
    static double minDistanceRad(const S2Point& point, const S2Polyline& line);
    static double minDistanceRad(const S2Point& point, const S2Polygon& polygon);
    static double minDistanceRad(const S2Point& point, const S2Cap& cap);
};

struct Point {
    Point();
    Point(double x, double y);
    explicit Point(const BSONElement& e);
    explicit Point(const BSONObj& o);
    std::string toString() const;

    double x;
    double y;
};

struct Circle {
    Circle();
    Circle(double radius, Point center);

    double radius;
    Point center;
};

class Box {
public:
    Box();
    Box(double x, double y, double size);
    Box(const Point& ptA, const Point& ptB);

    void init(const Point& ptA, const Point& ptB);
    void init(const Box& other);

    BSONArray toBSON() const;
    std::string toString() const;

    bool between(double min, double max, double val, double fudge = 0) const;
    bool onBoundary(double bound, double val, double fudge = 0) const;
    bool mid(double amin, double amax, double bmin, double bmax, bool min, double* res) const;

    double area() const;
    double maxDim() const;
    Point center() const;

    // NOTE: Box boundaries are *inclusive*
    bool onBoundary(Point p, double fudge = 0) const;
    bool inside(Point p, double fudge = 0) const;
    bool inside(double x, double y, double fudge = 0) const;
    bool contains(const Box& other, double fudge = 0) const;
    bool intersects(const Box& other) const;

    // Box modifications
    void truncate(double min, double max);
    void fudge(double error);
    void expandToInclude(const Point& pt);

    // TODO: Remove after 2D near dependency goes away
    double legacyIntersectFraction(const Box& other) const;

    Point _min;
    Point _max;
};

class Polygon {
public:
    Polygon();
    Polygon(const std::vector<Point>& points);

    void init(const std::vector<Point>& points);
    void init(const Polygon& other);

    int size() const;

    bool contains(const Point& p) const;

    /*
     * Return values:
     * -1 if no intersection
     * 0 if maybe an intersection (using fudge)
     * 1 if there is an intersection
     */
    int contains(const Point& p, double fudge) const;

    /**
     * Get the centroid of the polygon object.
     */
    const Point& centroid() const;
    const Box& bounds() const;
    const std::vector<Point>& points() const {
        return _points;
    }

private:
    // Only modified on creation and init()
    std::vector<Point> _points;

    // Cached attributes of the polygon
    mutable std::unique_ptr<Box> _bounds;
    mutable std::unique_ptr<Point> _centroid;
};

class R2Region {
public:
    virtual ~R2Region() {}

    virtual Box getR2Bounds() const = 0;

    /**
     * Fast heuristic containment check
     *
     * Returns true if the region definitely contains the box.
     * Returns false if not or if too expensive to find out one way or another.
     */
    virtual bool fastContains(const Box& other) const = 0;

    /**
     * Fast heuristic disjoint check
     *
     * Returns true if the region definitely is disjoint from the box.
     * Returns false if not or if too expensive to find out one way or another.
     */
    virtual bool fastDisjoint(const Box& other) const = 0;
};

// Annulus is used by GeoNear. Both inner and outer circles are inlcuded.
class R2Annulus : public R2Region {
public:
    R2Annulus();
    R2Annulus(const Point& center, double inner, double outer);

    const Point& center() const;

    double getInner() const;
    double getOuter() const;

    bool contains(const Point& point) const;

    // R2Region interface
    Box getR2Bounds() const;
    bool fastContains(const Box& other) const;
    bool fastDisjoint(const Box& other) const;

    // For debugging
    std::string toString() const;

private:
    Point _center;
    double _inner;
    double _outer;
};

// Clearly this isn't right but currently it's sufficient.
enum CRS {
    UNSET,
    FLAT,          // Equirectangular flat projection (i.e. trivial long/lat projection to flat map)
    SPHERE,        // WGS84
    STRICT_SPHERE  // WGS84 with strict winding order
};

// TODO: Make S2 less integral to these types - additional S2 shapes should be an optimization
// when our CRS is not projected, i.e. SPHERE for now.
// Generic shapes (Point, Line, Polygon) should hold the raw coordinate data - right now oldXXX
// is a misnomer - this is the *original* data and the S2 transformation just an optimization.

struct PointWithCRS {
    PointWithCRS() : crs(UNSET) {}

    S2Point point;
    S2Cell cell;
    Point oldPoint;
    CRS crs;
};

struct LineWithCRS {
    LineWithCRS() : crs(UNSET) {}

    S2Polyline line;
    CRS crs;
};

struct CapWithCRS {
    CapWithCRS() : crs(UNSET) {}

    S2Cap cap;
    Circle circle;
    CRS crs;
};

struct BoxWithCRS {
    BoxWithCRS() : crs(UNSET) {}

    Box box;
    CRS crs;
};

struct PolygonWithCRS {
    PolygonWithCRS() : crs(UNSET) {}

    std::unique_ptr<S2Polygon> s2Polygon;
    // Simple polygons with strict winding order may be bigger or smaller than a hemisphere.
    // Only used for query. We don't support storing/indexing big polygons.
    std::unique_ptr<BigSimplePolygon> bigPolygon;

    Polygon oldPolygon;
    CRS crs;
};

struct MultiPointWithCRS {
    MultiPointWithCRS() : crs(UNSET) {}

    std::vector<S2Point> points;
    std::vector<S2Cell> cells;
    CRS crs;
};

struct MultiLineWithCRS {
    MultiLineWithCRS() : crs(UNSET) {}

    OwnedPointerVector<S2Polyline> lines;
    CRS crs;
};

struct MultiPolygonWithCRS {
    MultiPolygonWithCRS() : crs(UNSET) {}

    OwnedPointerVector<S2Polygon> polygons;
    CRS crs;
};

struct GeometryCollection {
    std::vector<PointWithCRS> points;

    // The amount of indirection here is painful but we can't operator= unique_ptr or
    // OwnedPointerVector.
    OwnedPointerVector<LineWithCRS> lines;
    OwnedPointerVector<PolygonWithCRS> polygons;
    OwnedPointerVector<MultiPointWithCRS> multiPoints;
    OwnedPointerVector<MultiLineWithCRS> multiLines;
    OwnedPointerVector<MultiPolygonWithCRS> multiPolygons;

    bool supportsContains() {
        // Only polygons (and multiPolygons) support containment.
        return (polygons.vector().size() > 0 || multiPolygons.vector().size() > 0);
    }
};

//
// Projection functions - we only project following types for now
//   - Point
//   - Polygon (from STRICT_SPHERE TO SPHERE)
//
struct ShapeProjection {
    static bool supportsProject(const PointWithCRS& point, const CRS crs);
    static bool supportsProject(const PolygonWithCRS& polygon, const CRS crs);
    static void projectInto(PointWithCRS* point, CRS crs);
    static void projectInto(PolygonWithCRS* point, CRS crs);
};

}  // namespace mongo
