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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo::timeseries {
/**
 * Translates the given query on the time-series collection to a query on the time-series
 * collection's underlying buckets collection. Creates and returns a translated query document where
 * all occurrences of metaField in query are replaced with the literal "meta". Requires that the
 * given metaField is not empty.
 */
BSONObj translateQuery(const BSONObj& query, StringData metaField);

/**
 * Translates the given update on the time-series collection to an update on the time-series
 * collection's underlying buckets collection. Creates and returns a translated UpdateModification
 * where all occurrences of metaField in updateMod are replaced with the literal "meta". Requires
 * that updateMod is an update document and that the given metaField is not empty.
 */
write_ops::UpdateModification translateUpdate(const write_ops::UpdateModification& updateMod,
                                              StringData metaField);

/**
 * Returns the function to use to count the number of documents updated or deleted.
 */
std::function<size_t(const BSONObj&)> numMeasurementsForBucketCounter(StringData timeField);

/**
 * Translates the query into a query on the time-series collection's underlying buckets collection
 * and splits out the meta field predicate out of the query and renames it to 'meta'.
 */
BSONObj getBucketLevelPredicateForRouting(const BSONObj& originalQuery,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          boost::optional<StringData> metaField);
}  // namespace mongo::timeseries
