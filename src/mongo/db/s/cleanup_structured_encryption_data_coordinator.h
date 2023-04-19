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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/cleanup_structured_encryption_data_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"

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
        invariant(_response);
        return *_response;
    }

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final {}

private:
    StringData serializePhase(const Phase& phase) const override {
        return CleanupStructuredEncryptionDataPhase_serializer(phase);
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept final;

private:
    // The response to the cleanup command
    boost::optional<CleanupStructuredEncryptionDataCommandReply> _response;

    // Whether to skip the cleanup operation
    bool _skipCompact{false};

    // The UUID of the temporary collection that the ECOC was renamed to
    boost::optional<UUID> _ecocRenameUuid;

    // Stats for the ESC
    ECStats _escStats;

    // Stats for the ECOC
    ECOCStats _ecocStats;
};


}  // namespace mongo
