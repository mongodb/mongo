// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/compact_structured_encryption_data_coordinator_gen.h"
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
#include "mongo/util/uuid.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] CompactStructuredEncryptionDataCoordinator final
    : public RecoverableShardingDDLCoordinator<CompactStructuredEncryptionDataState> {
public:
    static constexpr auto kStateContext = "CompactStructuredEncryptionDataState"sv;

    CompactStructuredEncryptionDataCoordinator(ShardingCoordinatorService* service,
                                               const BSONObj& doc)
        : RecoverableShardingDDLCoordinator(
              service, "CompactStructuredEncryptionDataCoordinator", doc) {}

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

    CompactStructuredEncryptionDataCommandReply getResponse(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        tassert(10644504, "Expected _response to be set", _response);
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
    // The response to the compact command
    boost::optional<CompactStructuredEncryptionDataCommandReply> _response;

    // Whether to skip the compaction operation during the compact phase
    bool _skipCompact{false};

    // The UUID of the temporary collection that the ECOC was renamed to
    boost::optional<UUID> _ecocRenameUuid;

    // Contains the set of _id values of documents that must be deleted from the ESC
    // during the compact phase. This is populated during the rename phase.
    // It is by design that this is not persisted to disk between phases, as this should
    // be emptied (and hence no ESC deletions must happen) if the coordinator were resumed
    // from disk during the compact phase.
    FLECompactESCDeleteSet _escDeleteSet;

    // Stats for the ESC
    ECStats _escStats;

    // Stats for the ECOC
    ECOCStats _ecocStats;
};

}  // namespace mongo
