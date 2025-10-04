/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/global_catalog/ddl/compact_structured_encryption_data_coordinator_gen.h"
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
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class MONGO_MOD_PUB CompactStructuredEncryptionDataCoordinator final
    : public RecoverableShardingDDLCoordinator<CompactStructuredEncryptionDataState,
                                               CompactStructuredEncryptionDataPhaseEnum> {
public:
    static constexpr auto kStateContext = "CompactStructuredEncryptionDataState"_sd;
    using StateDoc = CompactStructuredEncryptionDataState;
    using Phase = CompactStructuredEncryptionDataPhaseEnum;

    CompactStructuredEncryptionDataCoordinator(ShardingDDLCoordinatorService* service,
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

private:
    StringData serializePhase(const Phase& phase) const override {
        return CompactStructuredEncryptionDataPhase_serializer(phase);
    }

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
