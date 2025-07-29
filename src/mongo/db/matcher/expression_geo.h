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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <ostream>
#include <string>

#include <s2cellid.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

struct PointWithCRS;
class GeometryContainer;

// This represents either a $within or a $geoIntersects.
class GeoExpression {
    GeoExpression(const GeoExpression&) = delete;
    GeoExpression& operator=(const GeoExpression&) = delete;

public:
    GeoExpression();
    GeoExpression(const std::string& f);

    enum Predicate { WITHIN, INTERSECT, INVALID };

    std::string getField() const {
        return _field;
    }
    Predicate getPred() const {
        return _predicate;
    }
    const GeometryContainer& getGeometry() const {
        return *_geoContainer;
    }
    std::shared_ptr<GeometryContainer> getGeometryPtr() const {
        return _geoContainer;
    }

    void setPredicate(Predicate predicate) {
        _predicate = predicate;
    }

    void setGeometry(std::shared_ptr<GeometryContainer> geoContainer) {
        _geoContainer = geoContainer;
    }

private:
    // Name of the field in the query.
    std::string _field;
    std::shared_ptr<GeometryContainer> _geoContainer;
    Predicate _predicate;
};

class GeoMatchExpression : public LeafMatchExpression {

public:
    GeoMatchExpression(boost::optional<StringData> path,
                       const GeoExpression* query,
                       const BSONObj& rawObj,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr);
    GeoMatchExpression(boost::optional<StringData> path,
                       std::shared_ptr<const GeoExpression> query,
                       const BSONObj& rawObj,
                       clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ~GeoMatchExpression() override {}

    void debugString(StringBuilder& debug, int indentationLevel = 0) const override;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const override;

    std::unique_ptr<MatchExpression> clone() const override;

    void setCanSkipValidation(bool val) {
        _canSkipValidation = val;
    }

    bool getCanSkipValidation() const {
        return _canSkipValidation;
    }

    const GeoExpression& getGeoExpression() const {
        return *_query;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const BSONObj& rawObjForHashing() const {
        return _rawObj;
    }

private:
    // The original geo specification provided by the user.
    BSONObj _rawObj;

    // Share ownership of our query with all of our clones
    std::shared_ptr<const GeoExpression> _query;
    bool _canSkipValidation;
};


// TODO: Make a struct, turn parse stuff into something like
// static Status parseNearQuery(const BSONObj& obj, NearQuery** out);
class GeoNearExpression {
    GeoNearExpression(const GeoNearExpression&) = delete;
    GeoNearExpression& operator=(const GeoNearExpression&) = delete;

public:
    GeoNearExpression();
    GeoNearExpression(const std::string& f);

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
};

class GeoNearMatchExpression : public LeafMatchExpression {
public:
    GeoNearMatchExpression(boost::optional<StringData> path,
                           const GeoNearExpression* query,
                           const BSONObj& rawObj);
    GeoNearMatchExpression(boost::optional<StringData> path,
                           std::shared_ptr<const GeoNearExpression> query,
                           const BSONObj& rawObj);

    ~GeoNearMatchExpression() override {}

    void debugString(StringBuilder& debug, int indentationLevel = 0) const override;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const override;

    std::unique_ptr<MatchExpression> clone() const override;

    const GeoNearExpression& getData() const {
        return *_query;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const BSONObj& rawObjForHashing() const {
        return _rawObj;
    }

private:
    // The original geo specification provided by the user.
    BSONObj _rawObj;

    // Share ownership of our query with all of our clones
    std::shared_ptr<const GeoNearExpression> _query;
};

/**
 * Expression which checks whether a legacy 2D index point is contained within our near
 * search annulus.  See nextInterval() below for more discussion.
 * TODO: Make this a standard type of GEO match expression
 */
class TwoDPtInAnnulusExpression : public LeafMatchExpression {
public:
    TwoDPtInAnnulusExpression(const R2Annulus& annulus, boost::optional<StringData> twoDPath)
        : LeafMatchExpression(INTERNAL_2D_POINT_IN_ANNULUS, twoDPath), _annulus(annulus) {}

    void serialize(BSONObjBuilder* out,
                   const SerializationOptions& opts = {},
                   bool includePath = true) const final {
        out->append("$TwoDPtInAnnulusExpression", true);
    }

    //
    // These won't be called.
    //

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const SerializationOptions& opts = {},
                                       bool includePath = true) const final {
        MONGO_UNREACHABLE_TASSERT(9911958);
    }

    void debugString(StringBuilder& debug, int level = 0) const final {
        MONGO_UNREACHABLE_TASSERT(9911959);
    }

    bool equivalent(const MatchExpression* other) const final {
        MONGO_UNREACHABLE_TASSERT(9911960);
        return false;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        MONGO_UNREACHABLE_TASSERT(9911961);
        return nullptr;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    R2Annulus getAnnulus() const {
        return _annulus;
    }

private:
    R2Annulus _annulus;
};
}  // namespace mongo
