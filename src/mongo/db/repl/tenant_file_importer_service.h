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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/tenant_file_cloner.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo::repl {
/**
 * Replica set aware service that runs both on the primary and secondaries. It orchestrates the
 * copying of data files from donor, import those files, and notifies the primary when the import is
 * successful.
 */
class TenantFileImporterService : public ReplicaSetAwareService<TenantFileImporterService> {
public:
    static constexpr StringData kTenantFileImporterServiceName = "TenantFileImporterService"_sd;
    static TenantFileImporterService* get(ServiceContext* serviceContext);
    static TenantFileImporterService* get(OperationContext* opCtx);
    TenantFileImporterService();

    using CreateConnectionFn = std::function<std::unique_ptr<DBClientConnection>()>;

    struct Stats {
        Date_t fileCopyStart;
        Date_t fileCopyEnd;
        uint64_t totalDataSize{0};
        uint64_t totalBytesCopied{0};
    };

    // Explicit State enum ordering defined here because we rely on comparison
    // operators for state checking in various TenantFileImporterService methods.
    enum class State {
        kUninitialized = 0,
        kStarted = 1,
        kLearnedFilename = 2,
        kLearnedAllFilenames = 3,
        kInterrupted = 4,
        kStopped = 5
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
            case State::kStopped:
                return "stopped";
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Begins the process of copying and importing files for a given migration.
     */
    void startMigration(const UUID& migrationId, const OpTime& startMigrationOpTime);

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
    void interruptMigration(const UUID& migrationId);

    /**
     * Resets the interrupted migration for the given migrationId by calling
     * _resetMigrationHandle(). See _resetMigrationHandle() for detailed comments.
     *
     * Throws an exception if called before the migration is interrupted.
     */
    void resetMigration(const UUID& migrationId);

    /**
     * Causes any in-progress migration be interrupted.
     */
    void interruptAll();

    /**
     * Returns a Future that will be resolved when the collection import task completes for the
     * given migration id. Return boost::none if no active migration matches the provided migration
     * id.
     */
    boost::optional<SharedSemiFuture<void>> getImportCompletedFuture(const UUID& migrationId);

    /**
     * Checks if there is an active migration with the given migration ID.
     */
    bool hasActiveMigration(const UUID& migrationId);

    /**
     * Returns the migration stats for the given migrationId.
     * If no migrationId is provided, it returns the stats of an ongoing migration, if any.
     */
    BSONObj getStats(boost::optional<const UUID&> migrationId = boost::none);
    void getStats(BSONObjBuilder& bob, boost::optional<const UUID&> migrationId = boost::none);

    void onConsistentDataAvailable(OperationContext*, bool, bool) final {}

    void onShutdown() final {
        {
            stdx::lock_guard lk(_mutex);
            // Prevents a new migration from starting up during or after shutdown.
            _isShuttingDown = true;
        }
        interruptAll();
        _resetMigrationHandle();
    }

    void onRollbackBegin() final {
        interruptAll();
        _resetMigrationHandle();
    }

    void onStartup(OperationContext*) final {}

    void onSetCurrentConfig(OperationContext* opCtx) final {}

    void onStepUpBegin(OperationContext*, long long) final {}

    void onStepUpComplete(OperationContext*, long long) final {}

    void onStepDown() final {}

    void onBecomeArbiter() final {}

    inline std::string getServiceName() const final {
        return "TenantFileImporterService";
    }

    /**
     * Set the function used to create a donor client connection. Used for testing.
     */
    void setCreateConnectionFn_forTest(const CreateConnectionFn& fn) {
        _createConnectionFn = fn;
    };

    /**
     * Returns the migrationId.
     */
    boost::optional<UUID> getMigrationId_forTest() {
        return _mh ? boost::make_optional(_mh->migrationId) : boost::none;
    }

    /**
     * Returns the migration state.
     */
    boost::optional<TenantFileImporterService::State> getState_forTest() {
        return _mh ? boost::make_optional(_mh->state) : boost::none;
    }

private:
    /**
     * A worker function that waits for ImporterEvents and handles cloning and importing files.
     */
    void _handleEvents(const UUID& migrationId);

    /**
     * Performs file copying from the donor for the specified filename in the given metadataDoc.
     */
    void _cloneFile(OperationContext* opCtx,
                    const UUID& migrationId,
                    DBClientConnection* clientConnection,
                    ThreadPool* workerPool,
                    TenantMigrationSharedData* sharedData,
                    const BSONObj& metadataDoc);

    /**
     * Waits until the majority committed StartMigrationTimestamp is successfully checkpointed.
     *
     * Note: Refer to the calling site for more information on its significance.
     */
    void _waitUntilStartMigrationTimestampIsCheckpointed(OperationContext* opCtx,
                                                         const UUID& migrationId);
    /**
     * Runs rollback to stable on the cloned files associated with the given migration id,
     * and then import the stable cloned files into the main WT instance.
     */
    void _runRollbackAndThenImportFiles(OperationContext* opCtx, const UUID& migrationId);

    /**
     * Called to inform the primary that we have finished copying and importing all files.
     */
    void _voteImportedFiles(OperationContext* opCtx, const UUID& migrationId);

    /**
     * Called internally by interrupt and interruptAll to interrupt a running file cloning and
     * import operations.
     */
    void _interrupt(WithLock lk, const UUID& migrationId);

    /**
     * This blocking call waits for the worker threads to finish the execution, and then releases
     * the resources held by MigrationHandle for the given migrationId (if provided) or for the
     * current ongoing migration.
     *
     * Throws an exception if called before the migration is interrupted.
     */
    void _resetMigrationHandle(boost::optional<const UUID&> migrationId = boost::none);

    /*
     * Transitions the migration associated with the given migrationId to the specified target
     * state. If dryRun is set to 'true', the function performs a dry run of the state transition
     * without actually changing the state. Throws an exception for an invalid state transition.
     *
     * Returns the current migration state before the state transition.
     */
    TenantFileImporterService::State _transitionToState(WithLock,
                                                        const UUID& migrationId,
                                                        State targetState,
                                                        bool dryRun = false);

    void _makeMigrationHandleIfNotPresent(WithLock,
                                          const UUID& migrationId,
                                          const OpTime& startMigrationOpTime);

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

    // Represents a handle for managing the migration process. It holds various resources and
    // information required for cloning files and importing them.
    struct MigrationHandle {
        explicit MigrationHandle(const UUID& migrationId, const OpTime& startMigrationOpTime);

        // Shard merge migration Id.
        const UUID migrationId;

        // Optime at which the recipient state machine document for this migration is initialized.
        const OpTime startMigrationOpTime;

        // Queue to process ImporterEvents.
        const std::unique_ptr<Queue> eventQueue;

        // ThreadPool used by TenantFileCloner to do storage write operations.
        const std::unique_ptr<ThreadPool> workerPool;

        // Shared between the importer service and TenantFileCloners
        const std::unique_ptr<TenantMigrationSharedData> sharedData;

        // Indicates if collection import for this migration has begun.
        bool importStarted = false;

        // Promise fulfilled upon completion of collection import for this migration.
        SharedPromise<void> importCompletedPromise;

        // Worker thread to orchestrate the cloning, importing and notifying the primary steps.
        std::unique_ptr<stdx::thread> workerThread;

        // State of the associated migration.
        State state = State::kUninitialized;

        // Tracks the Statistics of the associated migration.
        Stats stats;

        // Pointers below are not owned by this struct. The method that sets these
        // pointers must manage their lifecycle and ensure proper pointer reset to prevent
        // invalid memory access by other methods when reading the pointer value.

        // Donor DBClientConnection for file cloning.
        DBClientConnection* donorConnection = nullptr;

        // OperationContext associated with the migration.
        OperationContext* opCtx = nullptr;

        // Pointer to the current TenantFileCloner of the associated migration; used for statistics
        // purpose.
        TenantFileCloner* currentTenantFileCloner = nullptr;
    };

    stdx::mutex _mutex;

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

    std::unique_ptr<MigrationHandle> _mh;  // (M)

    // Used to create a new DBClientConnection to the donor.
    CreateConnectionFn _createConnectionFn = {};  // (W)

    // Condition variable to block concurrent reset operations.
    stdx::condition_variable _resetCV;  // (M)
    // Flag indicating whether a reset is currently in progress.
    bool _resetInProgress = false;  // (M)
};
}  // namespace mongo::repl
