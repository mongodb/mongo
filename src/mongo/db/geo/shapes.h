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
*/

#pragma once

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"

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
}  // namespace mongo
