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

#include <memory>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {
/**
 * Query for timeseries arbitrary writes should be split into two parts: bucket expression and
 * residual expression. The bucket expression is used to find the buckets and the residual
 * expression is used to filter the documents in the buckets.
 */
struct TimeseriesWritesQueryExprs {
    // The bucket-level match expression.
    std::unique_ptr<MatchExpression> _bucketExpr = nullptr;

    // The residual expression which is applied to materialized measurements after splitting out
    // bucket-level match expressions.
    std::unique_ptr<MatchExpression> _residualExpr = nullptr;
};

/**
 * Creates a TimeseriesWritesQueryExprs object if the collection is a time-series collection and
 * the related feature flag is enabled.
 */
inline std::unique_ptr<TimeseriesWritesQueryExprs> createTimeseriesWritesQueryExprsIfNecessary(
    bool featureEnabled, const CollectionPtr& collection) {
    if (featureEnabled && collection && collection->getTimeseriesOptions()) {
        return std::make_unique<TimeseriesWritesQueryExprs>();
    } else {
        return nullptr;
    }
}
}  // namespace mongo
