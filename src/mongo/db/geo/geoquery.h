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

#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2regionunion.h"

namespace mongo {

    class GeometryContainer {
    public:
        bool parseFrom(const BSONObj &obj);

        /**
         * To check intersection, we iterate over the otherContainer's geometries, checking each
         * geometry to see if we intersect it.  If we intersect one geometry, we intersect the
         * entire other container.
         */
        bool intersects(const GeometryContainer& otherContainer) const;

        /**
         * To check containment, we iterate over the otherContainer's geometries.  If we don't
         * contain any sub-geometry of the otherContainer, the otherContainer is not contained
         * within us.  If each sub-geometry of the otherContainer is contained within us, we contain
         * the entire otherContainer.
         */
        bool contains(const GeometryContainer& otherContainer) const;

        /**
         * Only polygons (and aggregate types thereof) support contains.
         */
        bool supportsContains() const;

        bool hasS2Region() const;

        // Used by s2cursor only to generate a covering of the query object.
        // One region is not NULL and this returns it.
        const S2Region& getRegion() const;
    private:
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
        shared_ptr<PointWithCRS> _point;
        shared_ptr<LineWithCRS> _line;
        shared_ptr<PolygonWithCRS> _polygon;
        shared_ptr<CapWithCRS> _cap;
        shared_ptr<MultiPointWithCRS> _multiPoint;
        shared_ptr<MultiLineWithCRS> _multiLine;
        shared_ptr<MultiPolygonWithCRS> _multiPolygon;
        shared_ptr<GeometryCollection> _geometryCollection;
        shared_ptr<BoxWithCRS> _box;

        shared_ptr<S2RegionUnion> _region;
    };

    class NearQuery {
    public:
        NearQuery() : minDistance(0), maxDistance(std::numeric_limits<double>::max()),
                      fromRadians(false) {}
        NearQuery(const string& f) : field(f), minDistance(0),
                                     maxDistance(std::numeric_limits<double>::max()),
                                     fromRadians(false) {}

        /**
         * If fromRadians is true after a parseFrom, minDistance and maxDistance are returned in
         * radians, not meters.  The distances must be multiplied by the underlying index's radius
         * to convert them to meters.
         *
         * This is annoying but useful when we don't know what index we're using at parse time.
         */
        bool parseFrom(const BSONObj &obj);
        bool parseFromGeoNear(const BSONObj &obj, double radius);

        string field;
        PointWithCRS centroid;

        // Min and max distance IN METERS from centroid that we're willing to search.
        double minDistance;
        double maxDistance;

        // Did we convert to this distance from radians?  (If so, we output distances in radians.)
        bool fromRadians;
    };

    // This represents either a $within or a $geoIntersects.
    class GeoQuery {
    public:
        GeoQuery() : field(""), predicate(INVALID) {}
        GeoQuery(const string& f) : field(f), predicate(INVALID) {}

        enum Predicate {
            WITHIN,
            INTERSECT,
            INVALID
        };

        bool parseFrom(const BSONObj &obj);
        bool satisfiesPredicate(const GeometryContainer &otherContainer) const;

        bool hasS2Region() const;
        const S2Region& getRegion() const;
        string getField() const { return field; }
    private:
        // Try to parse the provided object into the right place.
        bool parseLegacyQuery(const BSONObj &obj);
        bool parseNewQuery(const BSONObj &obj);

        // Name of the field in the query.
        string field;
        GeometryContainer geoContainer;
        Predicate predicate;
    };
}  // namespace mongo
