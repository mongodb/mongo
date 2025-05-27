/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/geo/shapes.h"

#include <memory>
#include <string>
#include <vector>

#include <s2.h>
#include <s2cell.h>
#include <s2cellid.h>
#include <s2polygon.h>
#include <s2polyline.h>
#include <s2region.h>
#include <s2regionunion.h>


namespace mongo {

class GeometryContainer {
public:
    /**
     * Creates an empty geometry container which may then be loaded from BSON or directly.
     */
    GeometryContainer() = default;

    GeometryContainer(const GeometryContainer&);
    GeometryContainer& operator=(const GeometryContainer&);

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
     * Returns the point data, if this geometry is a point.
     */
    PointWithCRS getPoint() const;

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

    BSONElement getGeoElement() const {
        return _geoElm;
    }

private:
    class R2BoxRegion;

    Status parseFromGeoJSON(bool skipValidation = false);

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

    // Only one of these clonable_ptrs should be non-NULL.  S2Region is a
    // superclass but it only supports testing against S2Cells.  We need
    // the most specific class we can get.
    clonable_ptr<PointWithCRS> _point;
    clonable_ptr<LineWithCRS> _line;
    clonable_ptr<BoxWithCRS> _box;
    clonable_ptr<PolygonWithCRS> _polygon;
    clonable_ptr<CapWithCRS> _cap;
    clonable_ptr<MultiPointWithCRS> _multiPoint;
    clonable_ptr<MultiLineWithCRS> _multiLine;
    clonable_ptr<MultiPolygonWithCRS> _multiPolygon;
    clonable_ptr<GeometryCollection> _geometryCollection;

    // Cached for use during covering calculations
    // TODO: _s2Region is currently generated immediately - don't necessarily need to do this
    std::unique_ptr<S2RegionUnion> _s2Region;
    std::unique_ptr<R2Region> _r2Region;

    BSONElement _geoElm;
};

/**
 * Structure that holds BSON addresses (BSONElements) and the corresponding geometry parsed
 * at those locations.
 * Used to separate the parsing of geometries from a BSONObj (which must stay in scope) from
 * the computation over those geometries.
 * TODO: Merge with 2D/2DSphere key extraction?
 */
class StoredGeometry {
public:
    static StoredGeometry* parseFrom(const BSONElement& element, bool skipValidation);

    static void extractGeometries(const BSONObj& doc,
                                  const string& path,
                                  std::vector<std::unique_ptr<StoredGeometry>>* geometries,
                                  bool skipValidation);

    BSONElement element;
    GeometryContainer geometry;
};

}  // namespace mongo
