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

#pragma once

#include <vector>

#include "mongo/db/geo/s2.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2loop.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polyline.h"
#include "third_party/s2/s2region.h"

namespace mongo {

// Simple GeoJSON polygon with a custom CRS identifier as having a strict winding order.
// The winding order will determine unambiguously the inside/outside of the polygon even
// if larger than one hemisphere.
//
// BigSimplePolygon uses S2Loop internally, which follows a left-foot rule (inside to the
// left when walking the edge of the polygon, counter-clockwise)
class BigSimplePolygon : public S2Region {
public:
    BigSimplePolygon();

    BigSimplePolygon(S2Loop* loop);

    virtual ~BigSimplePolygon();

    void Init(S2Loop* loop);

    double GetArea() const;

    bool Contains(const S2Polygon& polygon) const;

    bool Contains(const S2Polyline& line) const;

    // Needs to be this way for S2 compatibility
    bool Contains(S2Point const& point) const;

    bool Intersects(const S2Polygon& polygon) const;

    bool Intersects(const S2Polyline& line) const;

    bool Intersects(S2Point const& point) const;

    // Only used in tests
    void Invert();

    const S2Polygon& GetPolygonBorder() const;

    const S2Polyline& GetLineBorder() const;

    //
    // S2Region interface
    //

    BigSimplePolygon* Clone() const;

    S2Cap GetCapBound() const;

    S2LatLngRect GetRectBound() const;

    bool Contains(S2Cell const& cell) const;

    bool MayIntersect(S2Cell const& cell) const;

    bool VirtualContainsPoint(S2Point const& p) const;

    void Encode(Encoder* const encoder) const;

    bool Decode(Decoder* const decoder);

    bool DecodeWithinScope(Decoder* const decoder);

private:
    std::unique_ptr<S2Loop> _loop;

    // Cache whether the loop area is at most 2*Pi (the area of hemisphere).
    //
    // S2 guarantees that any loop in a valid (normalized) polygon, no matter a hole
    // or a shell, has to be less than 2*Pi. So if the loop is normalized, it's the same
    // with the border polygon, otherwise, the border polygon is its complement.
    bool _isNormalized;

    // Cached to do Intersects() and Contains() with S2Polylines.
    mutable std::unique_ptr<S2Polyline> _borderLine;
    mutable std::unique_ptr<S2Polygon> _borderPoly;
};
}
