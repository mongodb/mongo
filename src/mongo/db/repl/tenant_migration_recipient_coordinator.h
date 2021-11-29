/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/vote_commit_migration_progress_gen.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

namespace mongo::repl {

/**
 * Coordinate the nodes of the recipient shard during a multitenant migration that uses the "shard
 * merge" protocol.
 */
class TenantMigrationRecipientCoordinator {
public:
    static TenantMigrationRecipientCoordinator* get(ServiceContext* serviceContext);
    static TenantMigrationRecipientCoordinator* get(OperationContext* operationContext);

    TenantMigrationRecipientCoordinator() = default;
    ~TenantMigrationRecipientCoordinator() = default;

    // Begin a tenant migration step. The returned future is resolved when all replica set members
    // have completed the step, or there was an error.
    SharedSemiFuture<void> step(UUID migrationId, MigrationProgressStepEnum step);
    // TODO (SERVER-61144): use cancelStep, which is currently unused.
    void cancelStep(UUID migrationId, MigrationProgressStepEnum step);
    void voteForStep(UUID migrationId,
                     MigrationProgressStepEnum step,
                     const HostAndPort& host,
                     bool success,
                     const boost::optional<StringData>& reason = boost::none);
    void reset();

private:
    Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationRecipientCoordinator::_mutex");
    boost::optional<UUID> _currentMigrationId;
    MigrationProgressStepEnum _currentStep = MigrationProgressStepEnum::kNoStep;
    stdx::unordered_set<HostAndPort> _readyMembers;
    std::unique_ptr<SharedPromise<void>> _promise = std::make_unique<SharedPromise<void>>();
};
}  // namespace mongo::repl
