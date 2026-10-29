// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/query_analysis_op_observer_shardsvr.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

void QueryAnalysisOpObserverShardSvr::onUpdate(OperationContext* opCtx,
                                               const OplogUpdateEntryArgs& args,
                                               OpStateAccumulator* opAccumulator) {
    if (args.updateArgs->sampleId && opCtx->writesAreReplicated()) {
        updateWithSampleIdImpl(opCtx, args);
    }
}

}  // namespace analyze_shard_key
}  // namespace mongo
