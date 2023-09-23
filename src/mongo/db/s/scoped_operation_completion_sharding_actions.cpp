/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/s/scoped_operation_completion_sharding_actions.h"

#include <boost/optional.hpp>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const auto shardingOperationCompletionActionsRegistered =
    OperationContext::declareDecoration<bool>();

}  // namespace

ScopedOperationCompletionShardingActions::ScopedOperationCompletionShardingActions(
    OperationContext* opCtx)
    : _opCtx(opCtx) {
    if (!opCtx->getClient()->isInDirectClient()) {
        invariant(!shardingOperationCompletionActionsRegistered(_opCtx));
        shardingOperationCompletionActionsRegistered(_opCtx) = true;
    }
}

ScopedOperationCompletionShardingActions::~ScopedOperationCompletionShardingActions() noexcept {
    if (_opCtx->getClient()->isInDirectClient())
        return;

    shardingOperationCompletionActionsRegistered(_opCtx) = false;

    auto& oss = OperationShardingState::get(_opCtx);
    auto status = oss.resetShardingOperationFailedStatus();
    if (!status) {
        return;
    }

    if (auto staleInfo = status->extraInfo<StaleConfigInfo>()) {
        ShardingStatistics::get(_opCtx).countStaleConfigErrors.addAndFetch(1);
        bool inCriticalSection = staleInfo->getCriticalSectionSignal().has_value();
        bool stableLocalVersion = !inCriticalSection && staleInfo->getVersionWanted();

        if (stableLocalVersion &&
            ShardVersion::isPlacementVersionIgnored(staleInfo->getVersionReceived())) {
            // Shard is recovered, but the router didn't sent a shard version, therefore we just
            // need to tell the router how much it needs to advance to (getVersionWanted).
            return;
        }

        if (stableLocalVersion &&
            staleInfo->getVersionReceived().placementVersion().isOlderThan(
                staleInfo->getVersionWanted()->placementVersion())) {
            // Shard is recovered and the router is staler than the shard
            return;
        }

        if (inCriticalSection) {
            resharding_metrics::onCriticalSectionError(_opCtx, *staleInfo);
        }

        auto handleMismatchStatus = onCollectionPlacementVersionMismatchNoExcept(
            _opCtx, staleInfo->getNss(), staleInfo->getVersionReceived().placementVersion());
        if (!handleMismatchStatus.isOK())
            LOGV2(22053,
                  "Failed to handle stale version exception as part of the current operation: "
                  "{error}",
                  "Failed to handle stale version exception as part of the current operation",
                  "error"_attr = redact(handleMismatchStatus));
    } else if (auto staleInfo = status->extraInfo<StaleDbRoutingVersion>()) {
        bool stableLocalVersion =
            !staleInfo->getCriticalSectionSignal() && staleInfo->getVersionWanted();

        if (stableLocalVersion && staleInfo->getVersionReceived() < staleInfo->getVersionWanted()) {
            // Shard is recovered and the router is staler than the shard
            return;
        }

        auto handleMismatchStatus = onDbVersionMismatchNoExcept(
            _opCtx, staleInfo->getDb(), staleInfo->getVersionReceived());
        if (!handleMismatchStatus.isOK())
            LOGV2(22054,
                  "Failed to handle database version exception as part of the current operation: "
                  "{error}",
                  "Failed to database version exception as part of the current operation",
                  "error"_attr = redact(handleMismatchStatus));
    }
}

}  // namespace mongo
