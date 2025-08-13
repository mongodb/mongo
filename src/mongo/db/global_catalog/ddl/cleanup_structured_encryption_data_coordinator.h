/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/commands/fle2_cleanup_gen.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/global_catalog/ddl/cleanup_structured_encryption_data_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CleanupStructuredEncryptionDataCoordinator final
    : public RecoverableShardingDDLCoordinator<CleanupStructuredEncryptionDataState,
                                               CleanupStructuredEncryptionDataPhaseEnum> {
public:
    static constexpr auto kStateContext = "CleanupStructuredEncryptionDataState"_sd;
    using StateDoc = CleanupStructuredEncryptionDataState;
    using Phase = CleanupStructuredEncryptionDataPhaseEnum;

    CleanupStructuredEncryptionDataCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& doc)
        : RecoverableShardingDDLCoordinator(
              service, "CleanupStructuredEncryptionDataCoordinator", doc) {}

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

    CleanupStructuredEncryptionDataCommandReply getResponse(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        tassert(10644518, "Expected _response to be set", _response);
        return *_response;
    }

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final {}

private:
    StringData serializePhase(const Phase& phase) const override {
        return CleanupStructuredEncryptionDataPhase_serializer(phase);
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept final;

    std::set<NamespaceString> _getAdditionalLocksToAcquire(OperationContext* opCtx) override;

private:
    // Updates the cleanup stats in the state doc with the supplied stats by
    // adding onto the current stats in the state doc.
    void updateCleanupStats(const ECOCStats& phaseEcocStats, const ECStats& phaseEscStats);

    // The response to the cleanup command
    boost::optional<CleanupStructuredEncryptionDataCommandReply> _response;

    // Contains the set of _id values of non-anchor documents that must be deleted from the ESC
    // during the cleanup phase. This is populated during the rename phase.
    // It is by design that this is not persisted to disk between phases, as this should
    // be emptied (and hence no ESC deletions must happen) if the coordinator were resumed
    // from disk during the cleanup phase.
    FLECompactESCDeleteSet _escDeleteSet;

    // Priority queue of _id values of anchor documents that must be deleted from the ESC
    // during the "delete anchors" phase. This is populated during the cleanup phase.
    // It is by design that this is not persisted to disk between phases, as this should
    // be emptied if the coordinator were resumed from disk during the cleanup or anchor deletes
    // phase.
    FLECleanupESCDeleteQueue _escAnchorDeleteQueue;
};


}  // namespace mongo
