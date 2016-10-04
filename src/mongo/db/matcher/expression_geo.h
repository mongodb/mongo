// expression_geo.h

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


#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

struct PointWithCRS;
class GeometryContainer;

// This represents either a $within or a $geoIntersects.
class GeoExpression {
    MONGO_DISALLOW_COPYING(GeoExpression);

public:
    GeoExpression();
    GeoExpression(const std::string& f);

    enum Predicate { WITHIN, INTERSECT, INVALID };

    // parseFrom() must be called before getGeometry() to ensure initialization of geoContainer
    Status parseFrom(const BSONObj& obj);

    std::string getField() const {
        return field;
    }
    Predicate getPred() const {
        return predicate;
    }
    const GeometryContainer& getGeometry() const {
        return *geoContainer;
    }

private:
    // Parse geospatial query
    // e.g.
    // { "$intersect" : { "$geometry" : { "type" : "Point", "coordinates": [ 40, 5 ] } } }
    Status parseQuery(const BSONObj& obj);

    // Name of the field in the query.
    std::string field;
    std::unique_ptr<GeometryContainer> geoContainer;
    Predicate predicate;
};

class GeoMatchExpression : public LeafMatchExpression {
public:
    GeoMatchExpression() : LeafMatchExpression(GEO), _canSkipValidation(false) {}

    virtual ~GeoMatchExpression() {}

    /**
     * Takes ownership of the passed-in GeoExpression.
     */
    Status init(StringData path, const GeoExpression* query, const BSONObj& rawObj);

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const;

    void setCanSkipValidation(bool val) {
        _canSkipValidation = val;
    }

    bool getCanSkipValidation() const {
        return _canSkipValidation;
    }

    const GeoExpression& getGeoExpression() const {
        return *_query;
    }
    const BSONObj getRawObj() const {
        return _rawObj;
    }

private:
    BSONObj _rawObj;
    // Share ownership of our query with all of our clones
    std::shared_ptr<const GeoExpression> _query;
    bool _canSkipValidation;
};


// TODO: Make a struct, turn parse stuff into something like
// static Status parseNearQuery(const BSONObj& obj, NearQuery** out);
class GeoNearExpression {
    MONGO_DISALLOW_COPYING(GeoNearExpression);

public:
    GeoNearExpression();
    GeoNearExpression(const std::string& f);

    Status parseFrom(const BSONObj& obj);

    // The name of the field that contains the geometry.
    std::string field;

    // The starting point of the near search. Use forward declaration of geometries.
    std::unique_ptr<PointWithCRS> centroid;

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
    bool parseLegacyQuery(const BSONObj& obj);
    Status parseNewQuery(const BSONObj& obj);
};

class GeoNearMatchExpression : public LeafMatchExpression {
public:
    GeoNearMatchExpression() : LeafMatchExpression(GEO_NEAR) {}
    virtual ~GeoNearMatchExpression() {}

    Status init(StringData path, const GeoNearExpression* query, const BSONObj& rawObj);

    // This shouldn't be called and as such will crash.  GeoNear always requires an index.
    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    virtual std::unique_ptr<MatchExpression> shallowClone() const;

    const GeoNearExpression& getData() const {
        return *_query;
    }
    const BSONObj getRawObj() const {
        return _rawObj;
    }

private:
    BSONObj _rawObj;
    // Share ownership of our query with all of our clones
    std::shared_ptr<const GeoNearExpression> _query;
};

}  // namespace mongo
