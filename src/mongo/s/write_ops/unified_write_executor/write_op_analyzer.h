// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/router_role/routing_context.h"
#include "mongo/s/query/exec/target_write_op.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/write_ops/pause_migrations_during_multi_updates_enablement.h"
#include "mongo/s/write_ops/unified_write_executor/stats.h"
#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"
#include "mongo/s/write_ops/unified_write_executor/write_op.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

enum AnalysisType {
    kSingleShard,
    kMultiShard,
    kTwoPhaseWrite,
    kRetryableWriteWithId,
    kInternalTransaction,
    kMultiWriteBlockingMigrations,
};

struct Analysis {
    AnalysisType type;
    std::vector<ShardEndpoint> shardsAffected;
    // TODO SERVER-106874 remove the 'isViewfulTimeseries' flag entirely once 9.0 becomes last LTS.
    // By then we will only have viewless timeseries that do not require nss translation.
    //
    // 'isViewfulTimeseries' is set to true when the write op is on the main namespace of a viewful
    // timeseries collection. This flag makes sure the executor sends the command with translation
    // to buckets namespace correctly.
    bool isViewfulTimeseries = false;
    boost::optional<analyze_shard_key::TargetedSampleId> targetedSampleId;
    bool usesSVIgnored = false;
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
    /**
     * Record the targeting stats of the write op, this is only called for certain write types.
     */
    void recordTargetingStats(OperationContext* opCtx,
                              const CollectionRoutingInfo& cri,
                              const TargetOpResult& tr,
                              const WriteOp& op);

    Stats& _stats;
    PauseMigrationsDuringMultiUpdatesEnablement _pauseMigrationsDuringMultiUpdatesParameter;

    // Stores the result of 'makeTimeseriesBucketsNamespace()' to avoid re-making it.
    // TODO SERVER-106874 remove this once 9.0 becomes last LTS. By then we will only have viewless
    // timeseries that do not require nss translation.
    absl::flat_hash_map<NamespaceString, NamespaceString> _timeseriesBucketsNSSCache;
};

}  // namespace unified_write_executor
}  // namespace mongo
