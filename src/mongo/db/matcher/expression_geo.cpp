// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/matcher/expression_geo.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/matcher/expression_geo_serializer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include <s2cellid.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

//
// GeoExpression
//

// Put simple constructors here for unique_ptr.
GeoExpression::GeoExpression() : _field(""), _predicate(INVALID) {}
GeoExpression::GeoExpression(const std::string& f) : _field(f), _predicate(INVALID) {}

//
// GeoNearExpression
//

GeoNearExpression::GeoNearExpression()
    : minDistance(0),
      maxDistance(std::numeric_limits<double>::max()),
      isNearSphere(false),
      unitsAreRadians(false),
      isWrappingQuery(false) {}

GeoNearExpression::GeoNearExpression(const std::string& f)
    : field(f),
      minDistance(0),
      maxDistance(std::numeric_limits<double>::max()),
      isNearSphere(false),
      unitsAreRadians(false),
      isWrappingQuery(false) {}


//
// GeoMatchExpression and GeoNearMatchExpression
//

//
// Geo queries we don't need an index to answer: geoWithin and geoIntersects
//

/**
 * Takes ownership of the passed-in GeoExpression.
 */
GeoMatchExpression::GeoMatchExpression(boost::optional<std::string_view> path,
                                       const GeoExpression* query,
                                       const BSONObj& rawObj,
                                       clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(GEO, path, std::move(annotation)),
      _rawObj(rawObj),
      _query(query),
      _canSkipValidation(false),
      _2dsphereIndexVersion(boost::none) {}

/**
 * Takes shared ownership of the passed-in GeoExpression.
 */
GeoMatchExpression::GeoMatchExpression(boost::optional<std::string_view> path,
                                       std::shared_ptr<const GeoExpression> query,
                                       const BSONObj& rawObj,
                                       clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(GEO, path, std::move(annotation)),
      _rawObj(rawObj),
      _query(query),
      _canSkipValidation(false),
      _2dsphereIndexVersion(boost::none) {}

void GeoMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << "GEO raw = " << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

void GeoMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    if (!opts.isKeepingLiteralsUnchanged()) {
        geoExpressionCustomSerialization(*bob, _rawObj, opts, includePath);
        return;
    }
    bob->appendElements(_rawObj);
}

bool GeoMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const GeoMatchExpression* realOther = static_cast<const GeoMatchExpression*>(other);

    if (path() != realOther->path())
        return false;

    return SimpleBSONObjComparator::kInstance.evaluate(_rawObj == realOther->_rawObj) &&
        _2dsphereIndexVersion == realOther->_2dsphereIndexVersion;
}

std::unique_ptr<MatchExpression> GeoMatchExpression::clone() const {
    std::unique_ptr<GeoMatchExpression> next =
        std::make_unique<GeoMatchExpression>(path(), _query, _rawObj, _errorAnnotation);
    next->_canSkipValidation = _canSkipValidation;
    next->_2dsphereIndexVersion = _2dsphereIndexVersion;
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}

//
// Parse-only geo expressions: geoNear (formerly known as near).
//

GeoNearMatchExpression::GeoNearMatchExpression(boost::optional<std::string_view> path,
                                               const GeoNearExpression* query,
                                               const BSONObj& rawObj)
    : LeafMatchExpression(GEO_NEAR, path), _rawObj(rawObj), _query(query) {}

GeoNearMatchExpression::GeoNearMatchExpression(boost::optional<std::string_view> path,
                                               std::shared_ptr<const GeoNearExpression> query,
                                               const BSONObj& rawObj)
    : LeafMatchExpression(GEO_NEAR, path), _rawObj(rawObj), _query(query) {}


void GeoNearMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "GEONEAR " << _query->toString();
    _debugStringAttachTagInfo(&debug);
}

void GeoNearMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    if (!opts.isKeepingLiteralsUnchanged()) {
        geoNearExpressionCustomSerialization(*bob, _rawObj, opts, includePath);
        return;
    }
    bob->appendElements(_rawObj);
}

bool GeoNearMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const GeoNearMatchExpression* realOther = static_cast<const GeoNearMatchExpression*>(other);

    if (path() != realOther->path())
        return false;

    return SimpleBSONObjComparator::kInstance.evaluate(_rawObj == realOther->_rawObj);
}

std::unique_ptr<MatchExpression> GeoNearMatchExpression::clone() const {
    std::unique_ptr<GeoNearMatchExpression> next =
        std::make_unique<GeoNearMatchExpression>(path(), _query, _rawObj);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}
}  // namespace mongo
