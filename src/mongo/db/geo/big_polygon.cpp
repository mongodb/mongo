/**
 *    Copyright (C) 2014 10gen Inc.
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

#include "mongo/db/geo/big_polygon.h"

#include <map>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::unique_ptr;
using std::vector;


BigSimplePolygon::BigSimplePolygon() {}

// Caller should ensure loop is valid.
BigSimplePolygon::BigSimplePolygon(S2Loop* loop)
    : _loop(loop), _isNormalized(loop->IsNormalized()) {}

BigSimplePolygon::~BigSimplePolygon() {}

void BigSimplePolygon::Init(S2Loop* loop) {
    _loop.reset(loop);
    _isNormalized = loop->IsNormalized();
    _borderLine.reset();
    _borderPoly.reset();
}

double BigSimplePolygon::GetArea() const {
    return _loop->GetArea();
}

bool BigSimplePolygon::Contains(const S2Polygon& polygon) const {
    const S2Polygon& polyBorder = GetPolygonBorder();

    if (_isNormalized) {
        // Polygon border is the same as the loop
        return polyBorder.Contains(&polygon);
    }

    // Polygon border is the complement of the loop
    //
    // Return true iff big polygon's complement (polyBorder) doesn't intersect with polygon.
    // We don't guarantee whether the points on border are contained or not.
    return !polyBorder.Intersects(&polygon);
}

bool BigSimplePolygon::Contains(const S2Polyline& line) const {
    //
    // A line is contained within a loop if the result of subtracting the loop from the line is
    // nothing.
    //
    // Also, a line is contained within a loop if the result of clipping the line to the
    // complement of the loop is nothing.
    //
    // If we can't subtract the loop itself using S2, we clip (intersect) to the inverse.  Every
    // point in S2 is contained in exactly one of these loops.
    //
    // TODO: Polygon borders are actually kind of weird, and this is somewhat inconsistent with
    // Intersects().  A point might Intersect() a boundary exactly, but not be Contain()ed
    // within the Polygon.  Think the right thing to do here is custom intersection functions.
    //
    const S2Polygon& polyBorder = GetPolygonBorder();

    OwnedPointerVector<S2Polyline> clippedOwned;
    vector<S2Polyline*>& clipped = clippedOwned.mutableVector();

    if (_isNormalized) {
        // Polygon border is the same as the loop
        polyBorder.SubtractFromPolyline(&line, &clipped);
        return clipped.size() == 0;
    } else {
        // Polygon border is the complement of the loop
        polyBorder.IntersectWithPolyline(&line, &clipped);
        return clipped.size() == 0;
    }
}

bool BigSimplePolygon::Contains(S2Point const& point) const {
    return _loop->Contains(point);
}

bool BigSimplePolygon::Intersects(const S2Polygon& polygon) const {
    // If the loop area is at most 2*Pi, treat it as a simple Polygon.
    if (_isNormalized) {
        const S2Polygon& polyBorder = GetPolygonBorder();
        return polyBorder.Intersects(&polygon);
    }

    // The loop area is greater than 2*Pi, so it intersects a polygon (even with holes) if it
    // intersects any of the top-level polygon loops, since any valid polygon is less than
    // a hemisphere.
    //
    // Intersecting a polygon hole requires that the loop must have intersected the containing
    // loop - topology ftw.
    //
    // Another approach is to check polyBorder doesn't contain polygon, but the following
    // approach is cheaper.

    // Iterate over all the top-level polygon loops
    for (int i = 0; i < polygon.num_loops(); i = polygon.GetLastDescendant(i) + 1) {
        const S2Loop* polyLoop = polygon.loop(i);
        if (_loop->Intersects(polyLoop))
            return true;
    }

    return false;
}

bool BigSimplePolygon::Intersects(const S2Polyline& line) const {
    //
    // A loop intersects a line if line intersects the loop border or, if it doesn't, either
    // line is contained in the loop, or line is disjoint with the loop. So checking any
    // vertex of the line is sufficient.
    //
    // TODO: Make a general Polygon/Line relation tester which uses S2 primitives
    //
    return GetLineBorder().Intersects(&line) || _loop->Contains(line.vertex(0));
}

bool BigSimplePolygon::Intersects(S2Point const& point) const {
    return Contains(point);
}

void BigSimplePolygon::Invert() {
    _loop->Invert();
    _isNormalized = _loop->IsNormalized();
}

const S2Polygon& BigSimplePolygon::GetPolygonBorder() const {
    if (_borderPoly)
        return *_borderPoly;

    unique_ptr<S2Loop> cloned(_loop->Clone());

    // Any loop in polygon should be than a hemisphere (2*Pi).
    cloned->Normalize();

    OwnedPointerVector<S2Loop> loops;
    loops.mutableVector().push_back(cloned.release());
    _borderPoly.reset(new S2Polygon(&loops.mutableVector()));
    return *_borderPoly;
}

const S2Polyline& BigSimplePolygon::GetLineBorder() const {
    if (_borderLine)
        return *_borderLine;

    vector<S2Point> points;
    int numVertices = _loop->num_vertices();
    for (int i = 0; i <= numVertices; ++i) {
        // vertex() maps "numVertices" to 0 internally, so we don't have to deal with
        // the index out of range.
        points.push_back(_loop->vertex(i));
    }

    _borderLine.reset(new S2Polyline(points));

    return *_borderLine;
}

BigSimplePolygon* BigSimplePolygon::Clone() const {
    return new BigSimplePolygon(_loop->Clone());
}

S2Cap BigSimplePolygon::GetCapBound() const {
    return _loop->GetCapBound();
}

S2LatLngRect BigSimplePolygon::GetRectBound() const {
    return _loop->GetRectBound();
}

bool BigSimplePolygon::Contains(const S2Cell& cell) const {
    return _loop->Contains(cell);
}

bool BigSimplePolygon::MayIntersect(const S2Cell& cell) const {
    return _loop->MayIntersect(cell);
}

bool BigSimplePolygon::VirtualContainsPoint(const S2Point& p) const {
    return _loop->VirtualContainsPoint(p);
}

void BigSimplePolygon::Encode(Encoder* const encoder) const {
    invariant(false);
}

bool BigSimplePolygon::Decode(Decoder* const decoder) {
    invariant(false);
}

bool BigSimplePolygon::DecodeWithinScope(Decoder* const decoder) {
    invariant(false);
}
}
