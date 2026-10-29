// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

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
struct [[MONGO_MOD_FILE_PRIVATE]] TimeseriesTranslationParams {
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

[[MONGO_MOD_FILE_PRIVATE]] void prependUnpackStageToPipeline_forTest(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Pipeline& pipeline,
    const TimeseriesTranslationParams& params);


};  // namespace timeseries

}  // namespace mongo
