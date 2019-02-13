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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/scoped_operation_completion_sharding_actions.h"

#include "mongo/db/curop.h"
#include "mongo/db/s/implicit_create_collection.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

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
        auto handleMismatchStatus = onShardVersionMismatchNoExcept(
            _opCtx, staleInfo->getNss(), staleInfo->getVersionReceived());
        if (!handleMismatchStatus.isOK())
            log() << "Failed to handle stale version exception"
                  << causedBy(redact(handleMismatchStatus));
    } else if (auto cannotImplicitCreateCollInfo =
                   status->extraInfo<CannotImplicitlyCreateCollectionInfo>()) {
        if (ShardingState::get(_opCtx)->enabled()) {
            auto handleCannotImplicitCreateStatus =
                onCannotImplicitlyCreateCollection(_opCtx, cannotImplicitCreateCollInfo->getNss());
            if (!handleCannotImplicitCreateStatus.isOK())
                log() << "Failed to handle CannotImplicitlyCreateCollection exception"
                      << causedBy(redact(handleCannotImplicitCreateStatus));
        }
    }
}

}  // namespace mongo
