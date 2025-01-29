/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"

namespace mongo {
constexpr StringData InternalBucketGeoWithinMatchExpression::kName;

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
        _field == other->getField();
}

void InternalBucketGeoWithinMatchExpression::serialize(BSONObjBuilder* builder,
                                                       const SerializationOptions& opts,
                                                       bool includePath) const {
    BSONObjBuilder bob(builder->subobjStart(InternalBucketGeoWithinMatchExpression::kName));
    // Serialize the geometry shape.
    BSONObjBuilder withinRegionBob(
        bob.subobjStart(InternalBucketGeoWithinMatchExpression::kWithinRegion));
    opts.appendLiteral(&withinRegionBob, _geoContainer->getGeoElement());
    withinRegionBob.doneFast();
    // Serialize the field which is being searched over.
    bob.append(InternalBucketGeoWithinMatchExpression::kField,
               opts.serializeFieldPathFromString(_field));
    bob.doneFast();
}

std::unique_ptr<MatchExpression> InternalBucketGeoWithinMatchExpression::clone() const {
    std::unique_ptr<InternalBucketGeoWithinMatchExpression> next =
        std::make_unique<InternalBucketGeoWithinMatchExpression>(_geoContainer, _field);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}

}  // namespace mongo
