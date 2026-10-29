// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/query/exec/router_stage_queued_data.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

void RouterStageQueuedData::queueResult(ClusterQueryResult&& result) {
    const auto& resultObj = result.getResult();
    if (resultObj) {
        tassert(11052351, "Expected result object to be owned", resultObj->isOwned());
    }
    _resultsQueue.push({std::move(result)});
}

void RouterStageQueuedData::queueError(Status status) {
    _resultsQueue.push({status});
}

StatusWith<ClusterQueryResult> RouterStageQueuedData::next() {
    if (_resultsQueue.empty()) {
        return {ClusterQueryResult()};
    }

    auto out = std::move(_resultsQueue.front());
    _resultsQueue.pop();
    return out;
}

void RouterStageQueuedData::kill(OperationContext* opCtx) {
    // No child to kill.
}

bool RouterStageQueuedData::remotesExhausted() const {
    // No underlying remote cursor.
    return true;
}

std::size_t RouterStageQueuedData::getNumRemotes() const {
    return 0;
}

}  // namespace mongo
