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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/writer_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <list>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This class handles the execution part of the single document transformation aggregation stage
 * and is part of the execution pipeline. Its construction is based on
 * DocumentSourceSingleDocumentTransformation, which handles the optimization part.
 */
class OutStage final : public WriterStage<BSONObj> {
public:
    OutStage(StringData stageName,
             const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
             NamespaceString outputNs,
             const std::shared_ptr<TimeseriesOptions>& timeseries,
             boost::optional<ShardId> mergeShardId)
        : WriterStage<BSONObj>(stageName.data(), pExpCtx, std::move(outputNs)),
          _writeConcern(pExpCtx->getOperationContext()->getWriteConcern()),
          _timeseries(timeseries),
          _mergeShardId(std::move(mergeShardId)) {};

private:
    void doDispose() override;

    /**
     * Used to track the $out state for the destructor. $out should clean up different namespaces
     * depending on when the stage was interrupted or failed.
     */
    enum class OutCleanUpProgress {
        kTmpCollExists,
        kRenameComplete,
        kViewCreatedIfNeeded,
        kComplete
    };

    /**
     * Runs a createCollection command on the temporary namespace. Returns
     * nothing, but if the function returns, we assume the temporary collection is created.
     */
    void createTemporaryCollection();

    /**
     * Runs a renameCollection from the temporary namespace to the user requested namespace. Returns
     * nothing, but if the function returns, we assume the rename has succeeded and the temporary
     * namespace no longer exists.
     */
    void renameTemporaryCollection();

    /**
     * Runs a createCollection command to create the view backing the time-series buckets
     * collection. This should only be called if $out is writing to a time-series collection. If the
     * function returns, we assume the view is created.
     */
    void createTimeseriesView();

    void initialize() override;

    void finalize() override;

    void flush(BatchedCommandRequest bcr, BatchedObjects batch) override;

    std::pair<BSONObj, int> makeBatchObject(Document doc) const override {
        auto obj = doc.toBson();
        tassert(6628900, "_writeSizeEstimator should be initialized", _writeSizeEstimator);
        return {obj, _writeSizeEstimator->estimateInsertSizeBytes(obj)};
    }

    BatchedCommandRequest makeBatchedWriteRequest() const override;

    void waitWhileFailPointEnabled() override;

    NamespaceString makeBucketNsIfTimeseries(const NamespaceString& ns);

    /**
     * Determines if an error exists with the user input and existing collections. This function
     * sets the '_timeseries' member variable and must be run before referencing '_timeseries'
     * variable. The function will error if:
     * 1. The user provides the 'timeseries' field, but a non time-series collection or view exists
     * in that namespace.
     * 2. The user provides the 'timeseries' field with a specification that does not match an
     * existing time-series collection. The function will replace the value of '_timeseries' if the
     * user does not provide the 'timeseries' field, but a time-series collection exists.
     */
    std::shared_ptr<TimeseriesOptions> validateTimeseries();

    // Stash the writeConcern of the original command as the operation context may change by the
    // time we start to flush writes. This is because certain aggregations (e.g. $exchange)
    // establish cursors with batchSize 0 then run subsequent getMore's which use a new operation
    // context. The getMore's will not have an attached writeConcern however we still want to
    // respect the writeConcern of the original command.
    WriteConcernOptions _writeConcern;

    // Holds on to the original collection options and index specs so we can check they didn't
    // change during computation. For time-series collection these values will be on the buckets
    // namespace.
    BSONObj _originalOutOptions;
    std::list<BSONObj> _originalIndexes;

    // The temporary namespace for the $out writes.
    NamespaceString _tempNs;

    // Set if $out is writing to a time-series collection. This is how $out determines if it is
    // writing to a time-series collection or not. Any reference to this variable **must** be after
    // 'validateTimeseries', since 'validateTimeseries' sets this value.
    std::shared_ptr<TimeseriesOptions> _timeseries;

    // Tracks the current state of the temporary collection, and is used for cleanup.
    OutCleanUpProgress _tmpCleanUpState = OutCleanUpProgress::kComplete;

    // The UUID of the temporary output collection, used to detect if the temp collection UUID
    // changed during execution, which can cause incomplete results. This can happen if the primary
    // steps down during execution.
    boost::optional<UUID> _tempNsUUID = boost::none;

    boost::optional<ShardId> _mergeShardId;
};

}  // namespace mongo::exec::agg
