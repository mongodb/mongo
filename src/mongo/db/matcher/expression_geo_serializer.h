// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * We rely on these custom serializers for geo expressions to handle serialization with
 * kToRepresentativeParseableValue and kToDebugTypeString policies since the original raw query
 * needs to be re-parsed in order to properly serialize.
 *
 * Ideally each sub operator ($minDistance, $maxDistance, $geometry, $box) would serialize itself,
 * rather than GeoExpression reparse the query during serialization. However, GeoExpression and
 * GeoNearExpression don't capture the nesting of the various sub-operators. Re-parsing is therefore
 * required to serialize GeoMatchExpression and GeoNearMatchExpression into BSON representative of
 * the correct original query.
 *
 * To further complicate the serialization, serializing with policy
 * kToRepresentativeParseableValue requires output that can again be
 * re-parsed, and the geoparser performs validation checking to make sure input coordinates apply to
 * the correct geo type. For example, a GeoJSON Polygon must have minimum four pairs of coordinates
 * in a closed loop. The default representative parseable array value used in const
 * query_shape::SerializationOptions (an empty array) is not useful here since it won't pass geo
 * validation checks. As a workaround, this custom serializer determines a parseable value for each
 * shape or point type.
 */
void geoNearExpressionCustomSerialization(BSONObjBuilder& bob,
                                          const BSONObj& obj,
                                          const query_shape::SerializationOptions& opts = {},
                                          bool includePath = true);

void serializeGeoOperator(BSONObjBuilder& bob,
                          const BSONObj& obj,
                          const query_shape::SerializationOptions& opts = {});

void geoExpressionCustomSerialization(BSONObjBuilder& bob,
                                      const BSONObj& obj,
                                      const query_shape::SerializationOptions& opts = {},
                                      bool includePath = true);
}  // namespace mongo
