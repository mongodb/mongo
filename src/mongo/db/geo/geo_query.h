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

#include "mongo/db/geo/shapes.h"
#include "mongo/db/geo/geometry_container.h"

namespace mongo {

    // TODO: Make a struct, turn parse stuff into something like
    // static Status parseNearQuery(const BSONObj& obj, NearQuery** out);
    class NearQuery {
    public:
        NearQuery()
            : minDistance(0),
              maxDistance(std::numeric_limits<double>::max()),
              isNearSphere(false),
              unitsAreRadians(false),
              isWrappingQuery(false) { }

        NearQuery(const std::string& f)
            : field(f),
              minDistance(0),
              maxDistance(std::numeric_limits<double>::max()),
              isNearSphere(false),
              unitsAreRadians(false),
              isWrappingQuery(false) { }

        Status parseFrom(const BSONObj &obj);

        // The name of the field that contains the geometry.
        std::string field;

        // The starting point of the near search.
        PointWithCRS centroid;

        // Min and max distance from centroid that we're willing to search.
        // Distance is in units of the geometry's CRS, except SPHERE and isNearSphere => radians
        double minDistance;
        double maxDistance;

        // Is this a $nearSphere query
        bool isNearSphere;
        // $nearSphere with a legacy point implies units are radians
        bool unitsAreRadians;
        // $near with a non-legacy point implies a wrapping query, otherwise the query doesn't wrap
        bool isWrappingQuery;

        std::string toString() const {
            std::stringstream ss;
            ss << " field=" << field;
            ss << " maxdist=" << maxDistance;
            ss << " isNearSphere=" << isNearSphere;
            return ss.str();
        }

    private:
        bool parseLegacyQuery(const BSONObj &obj);
        Status parseNewQuery(const BSONObj &obj);
    };

    // This represents either a $within or a $geoIntersects.
    class GeoQuery {
    public:
        GeoQuery() : field(""), predicate(INVALID) {}
        GeoQuery(const std::string& f) : field(f), predicate(INVALID) {}

        enum Predicate {
            WITHIN,
            INTERSECT,
            INVALID
        };

        bool parseFrom(const BSONObj &obj);

        std::string getField() const { return field; }
        Predicate getPred() const { return predicate; }
        const GeometryContainer& getGeometry() const { return geoContainer; }

    private:
        // Try to parse the provided object into the right place.
        bool parseLegacyQuery(const BSONObj &obj);
        bool parseNewQuery(const BSONObj &obj);

        // Name of the field in the query.
        std::string field;
        GeometryContainer geoContainer;
        Predicate predicate;
    };
}  // namespace mongo
