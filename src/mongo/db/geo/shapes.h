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

#include <string>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/geo/s2.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2latlng.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polyline.h"

namespace mongo {

    struct Point;
    double distance(const Point& p1, const Point &p2);
    bool distanceWithin(const Point &p1, const Point &p2, double radius);
    void checkEarthBounds(const Point &p);
    double spheredist_rad(const Point& p1, const Point& p2);
    double spheredist_deg(const Point& p1, const Point& p2);

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

        double intersects(const Box& other) const;
        double area() const;
        double maxDim() const;
        Point center() const;

        bool onBoundary(Point p, double fudge = 0) const;
        bool inside(Point p, double fudge = 0) const;
        bool inside(double x, double y, double fudge = 0) const;
        bool contains(const Box& other, double fudge = 0) const;

        // Box modifications
        void truncate(double min, double max);
        void fudge(double error);
        void expandToInclude(const Point& pt);

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
        int contains(const Point &p, double fudge) const;

        /**
         * Get the centroid of the polygon object.
         */
        const Point& centroid() const;
        const Box& bounds() const;

    private:

        // Only modified on creation and init()
        std::vector<Point> _points;

        // Cached attributes of the polygon
        mutable scoped_ptr<Box> _bounds;
        mutable scoped_ptr<Point> _centroid;
    };

    class R2Region {
    public:

        virtual ~R2Region() {
        }

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

    // Clearly this isn't right but currently it's sufficient.
    enum CRS {
        UNSET,
        FLAT,
        SPHERE
    };

    struct PointWithCRS {

        PointWithCRS() : crs(UNSET), flatUpgradedToSphere(false) {}

        S2Point point;
        S2Cell cell;
        Point oldPoint;
        CRS crs;
        // If crs is FLAT, we might be able to upgrade the point to SPHERE if it's a valid SPHERE
        // point (lng/lat in bounds).  In this case, we can use FLAT data with SPHERE predicates.
        bool flatUpgradedToSphere;
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

        S2Polygon polygon;
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

        // The amount of indirection here is painful but we can't operator= scoped_ptr or
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

}  // namespace mongo
