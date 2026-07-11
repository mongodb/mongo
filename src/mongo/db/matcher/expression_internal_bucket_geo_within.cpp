// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"

#include "mongo/db/matcher/expression_geo_serializer.h"

#include <string_view>

namespace mongo {
constexpr std::string_view InternalBucketGeoWithinMatchExpression::kName;

void InternalBucketGeoWithinMatchExpression::debugString(StringBuilder& debug,
                                                         int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

bool InternalBucketGeoWithinMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalBucketGeoWithinMatchExpression*>(expr);

    return SimpleBSONObjComparator::kInstance.evaluate(
               _geoContainer->getGeoElement().Obj() ==
               other->getGeoContainer().getGeoElement().Obj()) &&
        _field == other->getField() && _indexVersion == other->_indexVersion;
}

void InternalBucketGeoWithinMatchExpression::serialize(
    BSONObjBuilder* builder,
    const query_shape::SerializationOptions& opts,
    bool includePath) const {
    BSONObjBuilder bob(builder->subobjStart(InternalBucketGeoWithinMatchExpression::kName));
    // Serialize the geometry shape.
    BSONObjBuilder withinRegionBob(
        bob.subobjStart(InternalBucketGeoWithinMatchExpression::kWithinRegion));
    if (!opts.isKeepingLiteralsUnchanged()) {
        serializeGeoOperator(withinRegionBob, _geoContainer->getGeoElement().wrap(), opts);
    } else {
        opts.appendLiteral(&withinRegionBob, _geoContainer->getGeoElement());
    }
    withinRegionBob.doneFast();
    // Serialize the field which is being searched over.
    bob.append(InternalBucketGeoWithinMatchExpression::kField,
               opts.serializeFieldPathFromString(_field));
    if (_indexVersion) {
        bob.append("2dsphereIndexVersion", static_cast<int>(*_indexVersion));
    }
    bob.doneFast();
}

std::unique_ptr<MatchExpression> InternalBucketGeoWithinMatchExpression::clone() const {
    std::unique_ptr<InternalBucketGeoWithinMatchExpression> next =
        std::make_unique<InternalBucketGeoWithinMatchExpression>(
            _geoContainer, _field, nullptr, _indexVersion);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}

}  // namespace mongo
