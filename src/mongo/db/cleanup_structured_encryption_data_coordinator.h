// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/cleanup_structured_encryption_data_coordinator_gen.h"
#include "mongo/db/commands/fle2_cleanup_gen.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;

class CleanupStructuredEncryptionDataCoordinator final
    : public RecoverableShardingDDLCoordinator<CleanupStructuredEncryptionDataState> {
public:
    static constexpr auto kStateContext = "CleanupStructuredEncryptionDataState"sv;

    CleanupStructuredEncryptionDataCoordinator(ShardingCoordinatorService* service,
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

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
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
