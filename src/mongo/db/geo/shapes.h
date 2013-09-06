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
#include "third_party/s2/s2.h"
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
        string toString() const;

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
        Box(Point min, Point max);

        BSONArray toBSON() const;
        string toString() const;

        bool between(double min, double max, double val, double fudge = 0) const;
        bool onBoundary(double bound, double val, double fudge = 0) const;
        bool mid(double amin, double amax, double bmin, double bmax, bool min, double* res) const;

        double intersects(const Box& other) const;
        double area() const;
        double maxDim() const;
        Point center() const;
        void truncate(double min, double max);
        void fudge(double error);
        bool onBoundary(Point p, double fudge = 0);
        bool inside(Point p, double fudge = 0) const;
        bool inside(double x, double y, double fudge = 0) const;
        bool contains(const Box& other, double fudge = 0);
        Point _min;
        Point _max;
    };

    class Polygon {
    public:
        Polygon();
        Polygon(vector<Point> points);

        void add(Point p);
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
         * Calculate the centroid, or center of mass of the polygon object.
         */
        Point centroid();
        Box bounds();
    private:
        bool _centroidCalculated;
        Point _centroid;
        Box _bounds;
        bool _boundsCalculated;
        vector<Point> _points;
    };

    // Clearly this isn't right but currently it's sufficient.
    enum CRS {
        FLAT,
        SPHERE
    };

    struct PointWithCRS {
        S2Point point;
        S2Cell cell;
        Point oldPoint;
        CRS crs;
    };

    struct LineWithCRS {
        S2Polyline line;
        CRS crs;
    };

    struct CapWithCRS {
        S2Cap cap;
        Circle circle;
        CRS crs;
    };

    struct BoxWithCRS {
        Box box;
        CRS crs;
    };

    struct PolygonWithCRS {
        S2Polygon polygon;
        Polygon oldPolygon;
        CRS crs;
    };

    struct MultiPointWithCRS {
        vector<S2Point> points;
        vector<S2Cell> cells;
        CRS crs;
    };

    struct MultiLineWithCRS {
        OwnedPointerVector<S2Polyline> lines;
        CRS crs;
    };

    struct MultiPolygonWithCRS {
        OwnedPointerVector<S2Polygon> polygons;
        CRS crs;
    };

    struct GeometryCollection {
        vector<PointWithCRS> points;

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
