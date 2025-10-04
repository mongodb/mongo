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

#include "mongo/base/status_with.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/router_role_api/routing_context.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/write_ops/pause_migrations_during_multi_updates_enablement.h"
#include "mongo/s/write_ops/unified_write_executor/stats.h"
#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"
#include "mongo/s/write_ops/unified_write_executor/write_op.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

enum BatchType {
    kSingleShard,
    kMultiShard,
    kNonTargetedWrite,
    kInternalTransaction,
    kMultiWriteBlockingMigrations,
};

struct Analysis {
    BatchType type;
    std::vector<ShardEndpoint> shardsAffected;
    boost::optional<analyze_shard_key::TargetedSampleId> targetedSampleId;
};

class WriteOpAnalyzer {
public:
    /**
     * Analyzes the given write op to determine which shards it would affect, and if it could be
     * combined into a batch with other writes.
     */
    virtual StatusWith<Analysis> analyze(OperationContext* opCtx,
                                         RoutingContext& routingCtx,
                                         const WriteOp& op) = 0;
};

class WriteOpAnalyzerImpl : public WriteOpAnalyzer {
public:
    WriteOpAnalyzerImpl(Stats& stats) : _stats(stats) {}

    /**
     * Analyzes the given write op to determine which shards it would affect, and if it could be
     * combined into a batch with other writes.
     */
    StatusWith<Analysis> analyze(OperationContext* opCtx,
                                 RoutingContext& routingCtx,
                                 const WriteOp& op) override;

private:
    Stats& _stats;
    PauseMigrationsDuringMultiUpdatesEnablement _pauseMigrationsDuringMultiUpdatesParameter;
};

}  // namespace unified_write_executor
}  // namespace mongo
