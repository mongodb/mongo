/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/geo/shapes.h"
#include "third_party/s2/s2regionunion.h"

namespace mongo {

class GeometryContainer {
    MONGO_DISALLOW_COPYING(GeometryContainer);

public:
    /**
     * Creates an empty geometry container which may then be loaded from BSON or directly.
     */
    GeometryContainer() = default;

    /**
     * Loads an empty GeometryContainer from query.
     */
    Status parseFromQuery(const BSONElement& elem);

    /**
     * Loads an empty GeometryContainer from stored geometry.
     */
    Status parseFromStorage(const BSONElement& elem, bool skipValidation = false);

    /**
     * Is the geometry any of {Point, Line, Polygon}?
     */
    bool isSimpleContainer() const;

    /**
     * Whether this geometry is a point
     */
    bool isPoint() const;

    /**
     * Reports the CRS of the contained geometry.
     * TODO: Rework once we have collections of multiple CRSes
     */
    CRS getNativeCRS() const;

    /**
     * Whether or not this geometry can be projected into a particular CRS
     */
    bool supportsProject(CRS crs) const;

    /**
     * Projects the current geometry into the supplied crs.
     * It is an error to call this function if canProjectInto(crs) is false.
     */
    void projectInto(CRS crs);

    /**
     * Minimum distance between this geometry and the supplied point.
     * TODO: Rework and generalize to full GeometryContainer distance
     */
    double minDistance(const PointWithCRS& point) const;

    /**
     * Only polygons (and aggregate types thereof) support contains.
     */
    bool supportsContains() const;

    /**
     * To check containment, we iterate over the otherContainer's geometries.  If we don't
     * contain any sub-geometry of the otherContainer, the otherContainer is not contained
     * within us.  If each sub-geometry of the otherContainer is contained within us, we contain
     * the entire otherContainer.
     */
    bool contains(const GeometryContainer& otherContainer) const;

    /**
     * To check intersection, we iterate over the otherContainer's geometries, checking each
     * geometry to see if we intersect it.  If we intersect one geometry, we intersect the
     * entire other container.
     */
    bool intersects(const GeometryContainer& otherContainer) const;

    // Region which can be used to generate a covering of the query object in the S2 space.
    bool hasS2Region() const;
    const S2Region& getS2Region() const;

    // Region which can be used to generate a covering of the query object in euclidean space.
    bool hasR2Region() const;
    const R2Region& getR2Region() const;

    // Returns a string related to the type of the geometry (for debugging queries)
    std::string getDebugType() const;

    // Needed for 2D wrapping check (for now)
    // TODO: Remove these hacks
    const CapWithCRS* getCapGeometryHack() const;

private:
    class R2BoxRegion;

    Status parseFromGeoJSON(const BSONObj& obj, bool skipValidation = false);

    // Does 'this' intersect with the provided type?
    bool intersects(const S2Cell& otherPoint) const;
    bool intersects(const S2Polyline& otherLine) const;
    bool intersects(const S2Polygon& otherPolygon) const;
    // These three just iterate over the geometries and call the 3 methods above.
    bool intersects(const MultiPointWithCRS& otherMultiPoint) const;
    bool intersects(const MultiLineWithCRS& otherMultiLine) const;
    bool intersects(const MultiPolygonWithCRS& otherMultiPolygon) const;

    // Used when 'this' has a polygon somewhere, either in _polygon or _multiPolygon or
    // _geometryCollection.
    bool contains(const S2Cell& otherCell, const S2Point& otherPoint) const;
    bool contains(const S2Polyline& otherLine) const;
    bool contains(const S2Polygon& otherPolygon) const;

    // Only one of these shared_ptrs should be non-NULL.  S2Region is a
    // superclass but it only supports testing against S2Cells.  We need
    // the most specific class we can get.
    std::unique_ptr<PointWithCRS> _point;
    std::unique_ptr<LineWithCRS> _line;
    std::unique_ptr<BoxWithCRS> _box;
    std::unique_ptr<PolygonWithCRS> _polygon;
    std::unique_ptr<CapWithCRS> _cap;
    std::unique_ptr<MultiPointWithCRS> _multiPoint;
    std::unique_ptr<MultiLineWithCRS> _multiLine;
    std::unique_ptr<MultiPolygonWithCRS> _multiPolygon;
    std::unique_ptr<GeometryCollection> _geometryCollection;

    // Cached for use during covering calculations
    // TODO: _s2Region is currently generated immediately - don't necessarily need to do this
    std::unique_ptr<S2RegionUnion> _s2Region;
    std::unique_ptr<R2Region> _r2Region;
};

}  // namespace mongo
