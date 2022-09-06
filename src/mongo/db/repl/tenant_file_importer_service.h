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

#include "boost/optional/optional.hpp"

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo::repl {
// Runs on tenant migration recipient primary and secondaries. Copies and imports donor files and
// then informs the primary that it has finished by running recipientVoteImportedFiles.
class TenantFileImporterService : public ReplicaSetAwareService<TenantFileImporterService> {
public:
    static constexpr StringData kTenantFileImporterServiceName = "TenantFileImporterService"_sd;
    static TenantFileImporterService* get(ServiceContext* serviceContext);
    TenantFileImporterService() = default;

    /**
     * Begins the process of copying and importing files for a given migration.
     */
    void startMigration(const UUID& migrationId);

    /**
     * Called for each file to be copied for a given migration.
     */
    void learnedFilename(const UUID& migrationId, const BSONObj& metadataDoc);

    /**
     * Called after all files have been copied for a given migration.
     */
    void learnedAllFilenames(const UUID& migrationId);

    /**
     * Interrupts an in-progress migration with the provided migration id.
     */
    void interrupt(const UUID& migrationId);

    /**
     * Causes any in-progress migration be interrupted.
     */
    void interruptAll();

private:
    void onInitialDataAvailable(OperationContext*, bool) final {}

    void onShutdown() final {
        {
            stdx::lock_guard lk(_mutex);
            // Prevents a new migration from starting up during or after shutdown.
            _isShuttingDown = true;
        }
        interruptAll();
        _reset();
    }

    void onStartup(OperationContext*) final {}

    void onStepUpBegin(OperationContext*, long long) final {}

    void onStepUpComplete(OperationContext*, long long) final {}

    void onStepDown() final {}

    void onBecomeArbiter() final {}

    /**
     * A worker function that waits for ImporterEvents and handles cloning and importing files.
     */
    void _handleEvents(const UUID& migrationId);

    /**
     * Called to inform the primary that we have finished copying and importing all files.
     */
    void _voteImportedFiles(OperationContext* opCtx, const UUID& migrationId);

    /**
     * Called internally by interrupt and interruptAll to interrupt a running file import operation.
     */
    void _interrupt(WithLock);

    /**
     * Waits for all async work to be finished and then resets internal state.
     */
    void _reset();

    // Explicit State enum ordering defined here because we rely on comparison
    // operators for state checking in various TenantFileImporterService methods.
    enum class State {
        kUninitialized = 0,
        kStarted = 1,
        kLearnedFilename = 2,
        kLearnedAllFilenames = 3,
        kInterrupted = 4
    };

    static StringData stateToString(State state) {
        switch (state) {
            case State::kUninitialized:
                return "uninitialized";
            case State::kStarted:
                return "started";
            case State::kLearnedFilename:
                return "learned filename";
            case State::kLearnedAllFilenames:
                return "learned all filenames";
            case State::kInterrupted:
                return "interrupted";
        }
        MONGO_UNREACHABLE;
        return StringData();
    }

    struct ImporterEvent {
        enum class Type { kNone, kLearnedFileName, kLearnedAllFilenames };
        Type type;
        UUID migrationId;
        BSONObj metadataDoc;

        ImporterEvent(Type _type, const UUID& _migrationId)
            : type(_type), migrationId(_migrationId) {}
    };

    using Queue =
        MultiProducerSingleConsumerQueue<ImporterEvent,
                                         producer_consumer_queue_detail::DefaultCostFunction>;
    Mutex _mutex = MONGO_MAKE_LATCH("TenantFileImporterService::_mutex");

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex.
    // (W)  Synchronization required only for writes.
    // (I)  Independently synchronized, see member variable comment.

    // Set to true when the shutdown procedure is initiated.
    bool _isShuttingDown = false;  // (M)

    OperationContext* _opCtx;  // (M)

    // The worker thread that processes ImporterEvents.
    std::unique_ptr<stdx::thread> _workerThread;  // (M)

    // The UUID of the current running migration.
    boost::optional<UUID> _migrationId;  // (M)

    // The state of the current running migration.
    State _state;  // (M)

    // The DBClientConnection to the donor used for cloning files.
    std::shared_ptr<DBClientConnection>
        _donorConnection;  // (I) pointer set under mutex, copied by callers.

    // The ThreadPool used for cloning files.
    std::shared_ptr<ThreadPool> _writerPool;  // (I) pointer set under mutex, copied by callers.

    // The TenantMigrationSharedData used for cloning files.
    std::shared_ptr<TenantMigrationSharedData>
        _sharedData;  // (I) pointer set under mutex, copied by callers.

    // The Queue used for processing ImporterEvents.
    std::shared_ptr<Queue> _eventQueue;  // (I) pointer set under mutex, copied by callers.
};
}  // namespace mongo::repl
