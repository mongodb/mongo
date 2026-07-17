// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <vector>

#include <s2.h>
#include <s2cap.h>
#include <s2cell.h>
#include <s2latlngrect.h>
#include <s2loop.h>
#include <s2polygon.h>
#include <s2polyline.h>
#include <s2region.h>

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

    ~BigSimplePolygon() override;

    BigSimplePolygon(const BigSimplePolygon&) = delete;
    BigSimplePolygon& operator=(const BigSimplePolygon&) = delete;

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

    //
    // S2Region interface
    //

    BigSimplePolygon* Clone() const override;

    S2Cap GetCapBound() const override;

    S2LatLngRect GetRectBound() const override;

    bool Contains(S2Cell const& cell) const override;

    bool MayIntersect(S2Cell const& cell) const override;

    bool VirtualContainsPoint(S2Point const& p) const override;

    void Encode(Encoder* encoder) const override;

    bool Decode(Decoder* decoder) override;

    bool DecodeWithinScope(Decoder* decoder) override;

private:
    std::unique_ptr<S2Loop> _loop;

    // Cache whether the loop area is at most 2*Pi (the area of hemisphere).
    //
    // S2 guarantees that any loop in a valid (normalized) polygon, no matter a hole
    // or a shell, has to be less than 2*Pi. So if the loop is normalized, it's the same
    // with the border polygon, otherwise, the border polygon is its complement.
    bool _isNormalized;

    // _borderPoly and _borderLine are lazily initialized caches used for spatial queries
    // against S2Polygon and S2Polyline respectively. Both the initialization and all S2
    // operations on these objects must be performed under _borderMu, because S2's internal
    // edge index (S2EdgeIndex) mutates shared state (query_count_) on every spatial query.
    // _borderMu also guards mutations of _loop and _isNormalized in Invert().
    mutable std::mutex _borderMu;
    mutable std::unique_ptr<S2Polyline> _borderLine;
    mutable std::unique_ptr<S2Polygon> _borderPoly;

    // Initialize _borderPoly if not already done. Caller must hold _borderMu.
    const S2Polygon& _getPolygonBorderLocked() const;

    // Initialize _borderLine if not already done. Caller must hold _borderMu.
    const S2Polyline& _getLineBorderLocked() const;
};
}  // namespace mongo
