/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"

namespace mongo {

namespace aggregation_hint_translation {

/**
 * Translates the index hint in the aggregation request if it exists and must be translated. The
 * only supported translation is for viewless timeseries collections.
 *
 * Viewless timeseries translations require accurate collection data from the global or shard
 * catalog. It is the caller's responsibility to ensure the catalog is accurate.
 */
template <class T>
inline void translateIndexHintIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const T& catalogData,
                                         AggregateCommandRequest& request) {
    timeseries::translateIndexHintIfRequired(expCtx, catalogData, request);
}
}  // namespace aggregation_hint_translation

}  // namespace mongo
