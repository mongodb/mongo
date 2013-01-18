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
*/

#include "mongo/db/geo/geoparser.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2regioncoverer.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2polyline.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2regioncoverer.h"

#pragma once

namespace mongo {
    class GeometryContainer {
    public:
        bool parseFrom(const BSONObj &obj);

        // Does we intersect the provided data?  Sadly there is no common good
        // way to check this, so we do different things for all pairs of
        // geometry_of(query,data).
        bool intersects(const GeometryContainer& otherContainer) const;
        bool intersects(const S2Cell& otherPoint) const;
        bool intersects(const S2Polyline& otherLine) const;
        bool intersects(const S2Polygon& otherPolygon) const;
        // And, within.
        bool contains(const GeometryContainer& otherContainer) const;

        bool supportsContains() const {
            return NULL != _polygon.get()
                   || NULL != _cap.get()
                   || NULL != _oldPolygon.get()
                   || NULL != _oldCircle.get();
        }

        bool hasS2Region() const {
            return NULL != _cell
                   || NULL != _line
                   || NULL != _polygon
                   || NULL != _cap;
        }

        // Used by s2cursor only to generate a covering of the query object.
        // One region is not NULL and this returns it.
        const S2Region& getRegion() const;
    private:
        // Only one of these shared_ptrs should be non-NULL.  S2Region is a
        // superclass but it only supports testing against S2Cells.  We need
        // the most specific class we can get.
        shared_ptr<S2Cell> _cell;
        shared_ptr<S2Polyline> _line;
        shared_ptr<S2Polygon> _polygon;
        shared_ptr<S2Cap> _cap;
        // Legacy shapes.
        shared_ptr<Polygon> _oldPolygon;
        shared_ptr<Box> _oldBox;
        shared_ptr<Circle> _oldCircle;
        shared_ptr<Point> _oldPoint;
    };

    class NearQuery {
    public:
        NearQuery() : maxDistance(std::numeric_limits<double>::max()), fromRadians(false) {}
        NearQuery(const string& f) : field(f), maxDistance(std::numeric_limits<double>::max()),
                                     fromRadians(false) {}
        bool parseFrom(const BSONObj &obj, double radius);
        bool parseFromGeoNear(const BSONObj &obj, double radius);
        string field;
        S2Point centroid;
        // Distance IN METERS that we're willing to search.
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
