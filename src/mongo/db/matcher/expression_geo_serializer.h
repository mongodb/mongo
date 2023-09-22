/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo {

/**
 * We rely on this custom serializer for geo expressions to handle serialization with
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
 * SerializationOptions (an empty array) is not useful here since it won't pass geo validation
 * checks. As a workaround, this custom serializer determines a parseable value for each shape or
 * point type.
 */
void geoCustomSerialization(BSONObjBuilder* bob,
                            const BSONObj& obj,
                            const SerializationOptions& opts);
}  // namespace mongo
