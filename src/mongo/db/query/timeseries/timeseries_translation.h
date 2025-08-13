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

#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"

namespace mongo {

/**
 * This namespace holds all of the functions to perform pre-optimization translations for
 * aggregations on viewless timeseries collections.
 *
 * These functions rely on collection metadata from the global or shard catalog. It is the
 * responsibility of the caller to validate that the catalog is not stale.
 *
 * If the pipeline needs to be translated, it is modified in-place. If a hint is present in the
 * aggregate command, the hint is also rewritten to use timeseries fields.
 */

namespace timeseries {

// TODO SERVER-101169 remove this once 'assumeNoMixedSchemaData' and 'areTimeseriesBucketsFixed' are
// in 'TimeseriesOptions'.
struct TimeseriesTranslationParams {
    const TimeseriesOptions& tsOptions;
    bool assumeNoMixedSchemaData = true;
    bool areTimeseriesBucketsFixed = false;
};

/**
 * Determine whether the request _requires_ viewless timeseries rewrites.
 *
 * Basic conditions: isRawDataOperation must return false and the collection must be a
 * viewless timeseries collection.
 */
bool requiresViewlessTimeseriesTranslation(OperationContext*, const CollectionOrViewAcquisition&);
bool requiresViewlessTimeseriesTranslationInRouter(OperationContext*, const CollectionRoutingInfo&);

/**
 * Modifies the document source container in-place if the pipeline requires viewless timeseries
 * translations. This function must have accurate catalog data to retrieve the timeseries options,
 * and it is the caller's responsibility to ensure the catalog data is accurate.
 */
void translateStagesIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               Pipeline& pipeline,
                               const CollectionRoutingInfo& cri);
void translateStagesIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               Pipeline& pipeline,
                               const CollectionOrViewAcquisition& collOrView);
/**
 * If an index hint exists and the request is on a viewless timeseries collection, this
 * translates the hint to use the timeseries bucket fields, and updates the hint in the
 * aggregate request.
 */
void translateIndexHintIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const CollectionOrViewAcquisition& collOrView,
                                  AggregateCommandRequest& request);
void translateIndexHintIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const CollectionRoutingInfo& cri,
                                  AggregateCommandRequest& request);

void prependUnpackStageToPipeline_forTest(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          Pipeline& pipeline,
                                          const TimeseriesTranslationParams& params);


};  // namespace timeseries

}  // namespace mongo
