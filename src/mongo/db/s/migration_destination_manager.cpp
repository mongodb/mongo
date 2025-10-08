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


#include "mongo/db/s/migration_batch_fetcher.h"
#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_destination_manager.h"

#include <list>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/start_chunk_clone_request.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangMigrationRecipientBeforeWaitingNoIndexBuildInProgress);

const auto getMigrationDestinationManager =
    ServiceContext::declareDecoration<MigrationDestinationManager>();

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by mongod and
                                                // writeConcernMajorityJournalDefault is set to true
                                                // in the ReplSetConfig.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoWaiting);

BSONObj makeLocalReadConcernWithAfterClusterTime(Timestamp afterClusterTime) {
    return BSON(repl::ReadConcernArgs::kReadConcernFieldName
                << BSON(repl::ReadConcernArgs::kLevelFieldName
                        << repl::readConcernLevels::kLocalName
                        << repl::ReadConcernArgs::kAfterClusterTimeFieldName << afterClusterTime));
}

void checkOutSessionAndVerifyTxnState(OperationContext* opCtx) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    mongoDSessionCatalog->checkOutUnscopedSession(opCtx);
    TransactionParticipant::get(opCtx).beginOrContinue(opCtx,
                                                       {*opCtx->getTxnNumber()},
                                                       boost::none /* autocommit */,
                                                       boost::none /* startTransaction */);
}

template <typename Callable>
constexpr bool returnsVoid() {
    return std::is_void_v<std::invoke_result_t<Callable>>;
}

// Yields the checked out session before running the given function. If the function runs without
// throwing, will reacquire the session and verify it is still valid to proceed with the migration.
template <typename Callable, std::enable_if_t<!returnsVoid<Callable>(), int> = 0>
auto runWithoutSession(OperationContext* opCtx, Callable&& callable) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    mongoDSessionCatalog->checkInUnscopedSession(opCtx,
                                                 OperationContextSession::CheckInReason::kYield);

    auto retVal = callable();

    // The below code can throw, so it cannot run in a scope guard.
    opCtx->checkForInterrupt();
    checkOutSessionAndVerifyTxnState(opCtx);

    return retVal;
}

// Same as runWithoutSession above but takes a void function.
template <typename Callable, std::enable_if_t<returnsVoid<Callable>(), int> = 0>
void runWithoutSession(OperationContext* opCtx, Callable&& callable) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    mongoDSessionCatalog->checkInUnscopedSession(opCtx,
                                                 OperationContextSession::CheckInReason::kYield);

    callable();

    // The below code can throw, so it cannot run in a scope guard.
    opCtx->checkForInterrupt();
    checkOutSessionAndVerifyTxnState(opCtx);
}

/**
 * Returns a human-readabale name of the migration manager's state.
 */
std::string stateToString(MigrationDestinationManager::State state) {
    switch (state) {
        case MigrationDestinationManager::kReady:
            return "ready";
        case MigrationDestinationManager::kClone:
            return "clone";
        case MigrationDestinationManager::kCatchup:
            return "catchup";
        case MigrationDestinationManager::kSteady:
            return "steady";
        case MigrationDestinationManager::kCommitStart:
            return "commitStart";
        case MigrationDestinationManager::kEnteredCritSec:
            return "enteredCriticalSection";
        case MigrationDestinationManager::kExitCritSec:
            return "exitCriticalSection";
        case MigrationDestinationManager::kDone:
            return "done";
        case MigrationDestinationManager::kFail:
            return "fail";
        case MigrationDestinationManager::kAbort:
            return "abort";
        default:
            MONGO_UNREACHABLE;
    }
}

bool isInRange(const BSONObj& obj,
               const BSONObj& min,
               const BSONObj& max,
               const BSONObj& shardKeyPattern) {
    ShardKeyPattern shardKey(shardKeyPattern);
    BSONObj k = shardKey.extractShardKeyFromDoc(obj);
    return k.woCompare(min) >= 0 && k.woCompare(max) < 0;
}

/**
 * Checks if an upsert of a remote document will override a local document with the same _id but in
 * a different range on this shard. Must be in WriteContext to avoid races and DBHelper errors.
 *
 * TODO: Could optimize this check out if sharding on _id.
 */
bool willOverrideLocalId(OperationContext* opCtx,
                         const NamespaceString& nss,
                         BSONObj min,
                         BSONObj max,
                         BSONObj shardKeyPattern,
                         BSONObj remoteDoc,
                         BSONObj* localDoc) {
    *localDoc = BSONObj();
    if (Helpers::findById(opCtx, nss, remoteDoc, *localDoc)) {
        return !isInRange(*localDoc, min, max, shardKeyPattern);
    }

    return false;
}

/**
 * Returns true if the majority of the nodes and the nodes corresponding to the given writeConcern
 * (if not empty) have applied till the specified lastOp.
 */
bool opReplicatedEnough(OperationContext* opCtx,
                        const repl::OpTime& lastOpApplied,
                        const WriteConcernOptions& writeConcern) {
    WriteConcernResult writeConcernResult;
    writeConcernResult.wTimedOut = false;

    Status majorityStatus =
        waitForWriteConcern(opCtx, lastOpApplied, kMajorityWriteConcern, &writeConcernResult);
    if (!majorityStatus.isOK()) {
        if (!writeConcernResult.wTimedOut) {
            uassertStatusOK(majorityStatus);
        }
        return false;
    }

    // Enforce the user specified write concern after "majority" so it covers the union of the 2
    // write concerns in case the user's write concern is stronger than majority
    WriteConcernOptions userWriteConcern(writeConcern);
    userWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcernResult.wTimedOut = false;

    Status userStatus =
        waitForWriteConcern(opCtx, lastOpApplied, userWriteConcern, &writeConcernResult);
    if (!userStatus.isOK()) {
        if (!writeConcernResult.wTimedOut) {
            uassertStatusOK(userStatus);
        }
        return false;
    }
    return true;
}

/**
 * Create the migration clone request BSON object to send to the source shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createMigrateCloneRequest(const NamespaceString& nss, const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_migrateClone", nss.ns());
    sessionId.append(&builder);
    return builder.obj();
}

/**
 * Create the migration transfer mods request BSON object to send to the source shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createTransferModsRequest(const NamespaceString& nss, const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_transferMods", nss.ns());
    sessionId.append(&builder);
    return builder.obj();
}

BSONObj criticalSectionReason(const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("recvChunk", 1);
    sessionId.append(&builder);
    return builder.obj();
}

bool migrationRecipientRecoveryDocumentExists(OperationContext* opCtx,
                                              const MigrationSessionId& sessionId) {
    PersistentTaskStore<MigrationRecipientRecoveryDocument> store(
        NamespaceString::kMigrationRecipientsNamespace);

    return store.count(opCtx,
                       BSON(MigrationRecipientRecoveryDocument::kMigrationSessionIdFieldName
                            << sessionId.toString())) > 0;
}

void replaceShardingIndexCatalogInShardIfNeeded(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const UUID& uuid) {
    auto currentShardHasAnyChunks = [&]() -> bool {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        const auto scsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
        const auto optMetadata = scsr->getCurrentMetadataIfKnown();
        return optMetadata && optMetadata->currentShardHasAnyChunks();
    }();

    // Early return, this shard already contains chunks, so there is no need for consolidate.
    if (currentShardHasAnyChunks) {
        return;
    }

    auto [_, optSii] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithIndexRefresh(opCtx, nss));

    if (optSii) {
        std::vector<IndexCatalogType> indexes;
        optSii->forEachIndex([&](const auto& index) {
            indexes.push_back(index);
            return true;
        });
        replaceCollectionShardingIndexCatalog(
            opCtx, nss, uuid, optSii->getCollectionIndexes().indexVersion(), indexes);
    } else {
        clearCollectionShardingIndexCatalog(opCtx, nss, uuid);
    }
}

// Enabling / disabling these fail points pauses / resumes MigrateStatus::_go(), the thread which
// receives a chunk migration from the donor.
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep1);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep2);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep3);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep4);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep5);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep6);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAfterSteadyTransition);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep7);

MONGO_FAIL_POINT_DEFINE(failMigrationOnRecipient);
MONGO_FAIL_POINT_DEFINE(failMigrationReceivedOutOfRangeOperation);
MONGO_FAIL_POINT_DEFINE(migrationRecipientFailPostCommitRefresh);

}  // namespace

const ReplicaSetAwareServiceRegistry::Registerer<MigrationDestinationManager> mdmRegistry(
    "MigrationDestinationManager");

MigrationDestinationManager::MigrationDestinationManager() = default;

MigrationDestinationManager::~MigrationDestinationManager() = default;

MigrationDestinationManager* MigrationDestinationManager::get(ServiceContext* serviceContext) {
    return &getMigrationDestinationManager(serviceContext);
}

MigrationDestinationManager* MigrationDestinationManager::get(OperationContext* opCtx) {
    return &getMigrationDestinationManager(opCtx->getServiceContext());
}

MigrationDestinationManager::State MigrationDestinationManager::getState() const {
    stdx::lock_guard<Latch> sl(_mutex);
    return _state;
}

void MigrationDestinationManager::_setState(State newState) {
    stdx::lock_guard<Latch> sl(_mutex);
    _state = newState;
    _stateChangedCV.notify_all();
}

void MigrationDestinationManager::_setStateFail(StringData msg) {
    LOGV2(21998,
          "Error during migration: {error}",
          "Error during migration",
          "error"_attr = redact(msg));
    {
        stdx::lock_guard<Latch> sl(_mutex);
        _errmsg = msg.toString();
        _state = kFail;
        _stateChangedCV.notify_all();
    }

    if (_sessionMigration) {
        _sessionMigration->forceFail(msg);
    }
}

void MigrationDestinationManager::_setStateFailWarn(StringData msg) {
    LOGV2_WARNING(22010,
                  "Error during migration: {error}",
                  "Error during migration",
                  "error"_attr = redact(msg));
    {
        stdx::lock_guard<Latch> sl(_mutex);
        _errmsg = msg.toString();
        _state = kFail;
        _stateChangedCV.notify_all();
    }

    if (_sessionMigration) {
        _sessionMigration->forceFail(msg);
    }
}

bool MigrationDestinationManager::isActive() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isActive(lk);
}

bool MigrationDestinationManager::_isActive(WithLock) const {
    return _sessionId.has_value();
}

void MigrationDestinationManager::report(BSONObjBuilder& b,
                                         OperationContext* opCtx,
                                         bool waitForSteadyOrDone) {
    if (waitForSteadyOrDone) {
        stdx::unique_lock<Latch> lock(_mutex);
        try {
            opCtx->waitForConditionOrInterruptFor(_stateChangedCV, lock, Seconds(1), [&]() -> bool {
                return _state != kReady && _state != kClone && _state != kCatchup;
            });
        } catch (...) {
            // Ignoring this error because this is an optional parameter and we catch timeout
            // exceptions later.
        }
        b.append("waited", true);
    }
    stdx::lock_guard<Latch> sl(_mutex);

    b.appendBool("active", _sessionId.has_value());

    if (_sessionId) {
        b.append("sessionId", _sessionId->toString());
    }

    b.append("ns", _nss.ns());
    b.append("from", _fromShardConnString.toString());
    b.append("fromShardId", _fromShard.toString());
    b.append("min", _min);
    b.append("max", _max);
    b.append("shardKeyPattern", _shardKeyPattern);
    b.append(StartChunkCloneRequest::kSupportsCriticalSectionDuringCatchUp, true);

    b.append("state", stateToString(_state));

    if (_state == kFail) {
        invariant(!_errmsg.empty());
        b.append("errmsg", _errmsg);
    }

    BSONObjBuilder bb(b.subobjStart("counts"));
    bb.append("cloned", _getNumCloned());
    bb.append("clonedBytes", _getNumBytesCloned());
    bb.append("catchup", _numCatchup);
    bb.append("steady", _numSteady);
    bb.done();
}

BSONObj MigrationDestinationManager::getMigrationStatusReport(
    const CollectionShardingRuntime::ScopedSharedCollectionShardingRuntime& scopedCsrLock) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_isActive(lk)) {
        boost::optional<long long> sessionOplogEntriesMigrated;
        if (_sessionMigration) {
            sessionOplogEntriesMigrated = _sessionMigration->getSessionOplogEntriesMigrated();
        }

        return migrationutil::makeMigrationStatusDocumentDestination(
            _nss, _fromShard, _toShard, false, _min, _max, sessionOplogEntriesMigrated);
    } else {
        return BSONObj();
    }
}

Status MigrationDestinationManager::start(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          ScopedReceiveChunk scopedReceiveChunk,
                                          const StartChunkCloneRequest& cloneRequest,
                                          const OID& epoch,
                                          const WriteConcernOptions& writeConcern) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_sessionId);
    invariant(!_scopedReceiveChunk);

    _state = kReady;
    _stateChangedCV.notify_all();
    _errmsg = "";

    _migrationId = cloneRequest.getMigrationId();
    _lsid = cloneRequest.getLsid();
    _txnNumber = cloneRequest.getTxnNumber();

    _parallelFetchersSupported = cloneRequest.parallelFetchingSupported();

    _nss = nss;
    _fromShard = cloneRequest.getFromShardId();
    _fromShardConnString =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, _fromShard))
            ->getConnString();
    _toShard = cloneRequest.getToShardId();
    _min = cloneRequest.getMinKey();
    _max = cloneRequest.getMaxKey();
    _shardKeyPattern = cloneRequest.getShardKeyPattern();

    _epoch = epoch;

    _writeConcern = writeConcern;

    _chunkMarkedPending = false;

    _migrationCloningProgress = std::make_shared<MigrationCloningProgressSharedState>();

    _numCatchup = 0;
    _numSteady = 0;

    _sessionId = cloneRequest.getSessionId();
    _scopedReceiveChunk = std::move(scopedReceiveChunk);

    invariant(!_canReleaseCriticalSectionPromise);
    _canReleaseCriticalSectionPromise = std::make_unique<SharedPromise<void>>();

    invariant(!_migrateThreadFinishedPromise);
    _migrateThreadFinishedPromise = std::make_unique<SharedPromise<State>>();

    // TODO: If we are here, the migrate thread must have completed, otherwise _active above
    // would be false, so this would never block. There is no better place with the current
    // implementation where to join the thread.
    if (_migrateThreadHandle.joinable()) {
        _migrateThreadHandle.join();
    }

    _sessionMigration = std::make_unique<SessionCatalogMigrationDestination>(
        _nss, _fromShard, *_sessionId, _cancellationSource.token());
    ShardingStatistics::get(opCtx).countRecipientMoveChunkStarted.addAndFetch(1);

    _migrateThreadHandle = stdx::thread([this, cancellationToken = _cancellationSource.token()]() {
        _migrateThread(cancellationToken);
    });

    return Status::OK();
}

Status MigrationDestinationManager::restoreRecoveredMigrationState(
    OperationContext* opCtx,
    ScopedReceiveChunk scopedReceiveChunk,
    const MigrationRecipientRecoveryDocument& recoveryDoc) {
    stdx::lock_guard<Latch> sl(_mutex);
    invariant(!_sessionId);

    _scopedReceiveChunk = std::move(scopedReceiveChunk);
    _nss = recoveryDoc.getNss();
    _migrationId = recoveryDoc.getId();
    _sessionId = recoveryDoc.getMigrationSessionId();
    _min = recoveryDoc.getRange().getMin();
    _max = recoveryDoc.getRange().getMax();
    _lsid = recoveryDoc.getLsid();
    _txnNumber = recoveryDoc.getTxnNumber();
    _state = kCommitStart;

    invariant(!_canReleaseCriticalSectionPromise);
    _canReleaseCriticalSectionPromise = std::make_unique<SharedPromise<void>>();

    invariant(!_migrateThreadFinishedPromise);
    _migrateThreadFinishedPromise = std::make_unique<SharedPromise<State>>();

    LOGV2(6064500, "Recovering migration recipient", "sessionId"_attr = *_sessionId);

    if (_migrateThreadHandle.joinable()) {
        _migrateThreadHandle.join();
    }

    _migrateThreadHandle = stdx::thread([this, cancellationToken = _cancellationSource.token()]() {
        _migrateThread(cancellationToken, true /* skipToCritSecTaken */);
    });

    return Status::OK();
}

repl::OpTime MigrationDestinationManager::fetchAndApplyBatch(
    OperationContext* opCtx,
    std::function<bool(OperationContext*, BSONObj)> applyBatchFn,
    std::function<bool(OperationContext*, BSONObj*)> fetchBatchFn) {

    SingleProducerSingleConsumerQueue<BSONObj>::Options options;
    options.maxQueueDepth = 1;

    SingleProducerSingleConsumerQueue<BSONObj> batches(options);
    repl::OpTime lastOpApplied;

    stdx::thread applicationThread{[&] {
        Client::initThread("batchApplier", opCtx->getServiceContext(), nullptr);
        auto client = Client::getCurrent();
        {
            stdx::lock_guard lk(*client);
            client->setSystemOperationKillableByStepdown(lk);
        }
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
        auto applicationOpCtx = CancelableOperationContext(
            cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

        ScopeGuard consumerGuard([&] {
            batches.closeConsumerEnd();
            lastOpApplied =
                repl::ReplClientInfo::forClient(applicationOpCtx->getClient()).getLastOp();
        });

        try {
            while (true) {
                DisableDocumentValidation documentValidationDisabler(
                    applicationOpCtx.get(),
                    DocumentValidationSettings::kDisableSchemaValidation |
                        DocumentValidationSettings::kDisableInternalValidation);
                auto nextBatch = batches.pop(applicationOpCtx.get());
                if (!applyBatchFn(applicationOpCtx.get(), nextBatch)) {
                    return;
                }
            }
        } catch (...) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            opCtx->getServiceContext()->killOperation(lk, opCtx, ErrorCodes::Error(51008));
            LOGV2(21999,
                  "Batch application failed: {error}",
                  "Batch application failed",
                  "error"_attr = redact(exceptionToStatus()));
        }
    }};


    {
        ScopeGuard applicationThreadJoinGuard([&] {
            batches.closeProducerEnd();
            applicationThread.join();
        });

        while (true) {
            BSONObj nextBatch;
            bool emptyBatch = fetchBatchFn(opCtx, &nextBatch);
            try {
                batches.push(nextBatch.getOwned(), opCtx);
                if (emptyBatch) {
                    break;
                }
            } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>&) {
                break;
            }
        }
    }  // This scope ensures that the guard is destroyed

    // This check is necessary because the consumer thread uses killOp to propagate errors to the
    // producer thread (this thread)
    opCtx->checkForInterrupt();
    return lastOpApplied;
}

Status MigrationDestinationManager::abort(const MigrationSessionId& sessionId) {
    stdx::lock_guard<Latch> sl(_mutex);

    if (!_sessionId) {
        return Status::OK();
    }

    if (!_sessionId->matches(sessionId)) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "received abort request from a stale session "
                              << sessionId.toString() << ". Current session is "
                              << _sessionId->toString()};
    }

    _state = kAbort;
    _stateChangedCV.notify_all();
    _errmsg = "aborted";

    return Status::OK();
}

void MigrationDestinationManager::abortWithoutSessionIdCheck() {
    stdx::lock_guard<Latch> sl(_mutex);
    _state = kAbort;
    _stateChangedCV.notify_all();
    _errmsg = "aborted without session id check";
}

Status MigrationDestinationManager::startCommit(const MigrationSessionId& sessionId) {
    stdx::unique_lock<Latch> lock(_mutex);

    const auto convergenceTimeout =
        Shard::kDefaultConfigCommandTimeout + Shard::kDefaultConfigCommandTimeout / 4;

    // The donor may have started the commit while the recipient is still busy processing
    // the last batch of mods sent in the catch up phase. Allow some time for synching up.
    auto deadline = Date_t::now() + convergenceTimeout;

    while (_state == kCatchup) {
        if (stdx::cv_status::timeout ==
            _stateChangedCV.wait_until(lock, deadline.toSystemTimePoint())) {
            return {ErrorCodes::CommandFailed,
                    str::stream() << "startCommit timed out waiting for the catch up completion. "
                                  << "Sender's session is " << sessionId.toString()
                                  << ". Current session is "
                                  << (_sessionId ? _sessionId->toString() : "none.")};
        }
    }

    if (_state != kSteady) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "Migration startCommit attempted when not in STEADY state."
                              << " Sender's session is " << sessionId.toString()
                              << (_sessionId ? (". Current session is " + _sessionId->toString())
                                             : ". No active session on this shard.")};
    }

    // In STEADY state we must have active migration
    invariant(_sessionId);

    // This check guards against the (unusual) situation where the current donor shard has stalled,
    // during which the recipient shard crashed or timed out, and then began serving as a recipient
    // or donor for another migration.
    if (!_sessionId->matches(sessionId)) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "startCommit received commit request from a stale session "
                              << sessionId.toString() << ". Current session is "
                              << _sessionId->toString()};
    }

    _sessionMigration->finish();
    _state = kCommitStart;
    _stateChangedCV.notify_all();

    // Assigning a timeout slightly higher than the one used for network requests to the config
    // server. Enough time to retry at least once in case of network failures (SERVER-51397).
    deadline = Date_t::now() + convergenceTimeout;

    while (_state == kCommitStart) {
        if (stdx::cv_status::timeout ==
            _stateChangedCV.wait_until(lock, deadline.toSystemTimePoint())) {
            _errmsg = str::stream() << "startCommit timed out waiting, " << _sessionId->toString();
            _state = kFail;
            _stateChangedCV.notify_all();
            return {ErrorCodes::CommandFailed, _errmsg};
        }
    }
    if (_state != kEnteredCritSec) {
        return {ErrorCodes::CommandFailed,
                "startCommit failed, final data failed to transfer or failed to enter critical "
                "section"};
    }

    return Status::OK();
}

Status MigrationDestinationManager::exitCriticalSection(OperationContext* opCtx,
                                                        const MigrationSessionId& sessionId) {
    SharedSemiFuture<State> threadFinishedFuture;
    {
        stdx::unique_lock<Latch> lock(_mutex);
        if (!_sessionId || !_sessionId->matches(sessionId)) {
            LOGV2_DEBUG(5899104,
                        2,
                        "Request to exit recipient critical section does not match current session",
                        "requested"_attr = sessionId,
                        "current"_attr = _sessionId);

            // No need to hold _mutex from here on. Release it because the lines below will acquire
            // other locks and holding the mutex could lead to deadlocks.
            lock.unlock();

            if (migrationRecipientRecoveryDocumentExists(opCtx, sessionId)) {
                // This node may have stepped down and interrupted the migrateThread, which reset
                // _sessionId. But the critical section may not have been released so it will be
                // recovered by the new primary.
                return {ErrorCodes::CommandFailed,
                        "Recipient migration recovery document still exists"};
            }

            // Ensure the command's wait for writeConcern will until the recovery document is
            // deleted.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

            return Status::OK();
        }

        if (_state < kEnteredCritSec) {
            return {ErrorCodes::CommandFailed,
                    "recipient critical section has not yet been entered"};
        }

        // Fulfill the promise to let the migrateThread release the critical section.
        invariant(_canReleaseCriticalSectionPromise);
        if (!_canReleaseCriticalSectionPromise->getFuture().isReady()) {
            _canReleaseCriticalSectionPromise->emplaceValue();
        }

        threadFinishedFuture = _migrateThreadFinishedPromise->getFuture();
    }

    // Wait for the migrateThread to finish
    const auto threadFinishState = threadFinishedFuture.get(opCtx);

    if (threadFinishState != kDone) {
        return {ErrorCodes::CommandFailed, "exitCriticalSection failed"};
    }

    LOGV2_DEBUG(
        5899105, 2, "Succeeded releasing recipient critical section", "requested"_attr = sessionId);

    return Status::OK();
}

MigrationDestinationManager::IndexesAndIdIndex MigrationDestinationManager::getCollectionIndexes(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    const ShardId& fromShardId,
    const boost::optional<CollectionRoutingInfo>& cri,
    boost::optional<Timestamp> afterClusterTime) {
    auto fromShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, fromShardId));

    std::vector<BSONObj> donorIndexSpecs;
    BSONObj donorIdIndexSpec;

    // Get the collection indexes and options from the donor shard.

    // Do not hold any locks while issuing remote calls.
    invariant(!opCtx->lockState()->isLocked());

    auto cmd = nssOrUUID.isNamespaceString() ? BSON("listIndexes" << nssOrUUID.nss().coll())
                                             : BSON("listIndexes" << nssOrUUID.uuid());
    if (cri) {
        cmd = appendShardVersion(cmd, cri->getShardVersion(fromShardId));
    }
    if (afterClusterTime) {
        cmd = cmd.addFields(makeLocalReadConcernWithAfterClusterTime(*afterClusterTime));
    }

    // Get indexes by calling listIndexes against the donor.
    auto indexes = uassertStatusOK(
        fromShard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                              nssOrUUID.dbName().toString(),
                                              cmd,
                                              Milliseconds(-1)));

    for (auto&& spec : indexes.docs) {
        if (spec[IndexDescriptor::kClusteredFieldName]) {
            // The 'clustered' index is implicitly created upon clustered collection creation.
        } else {
            donorIndexSpecs.push_back(spec);
            if (auto indexNameElem = spec[IndexDescriptor::kIndexNameFieldName]) {
                if (indexNameElem.type() == BSONType::String &&
                    indexNameElem.valueStringData() == "_id_"_sd) {
                    donorIdIndexSpec = spec;
                }
            }
        }
    }

    return {donorIndexSpecs, donorIdIndexSpec};
}


MigrationDestinationManager::CollectionOptionsAndUUID
MigrationDestinationManager::getCollectionOptions(OperationContext* opCtx,
                                                  const NamespaceStringOrUUID& nssOrUUID,
                                                  boost::optional<Timestamp> afterClusterTime) {
    const auto dbInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nssOrUUID.dbName().toString()));
    return getCollectionOptions(
        opCtx, nssOrUUID, dbInfo->getPrimary(), dbInfo->getVersion(), afterClusterTime);
}

MigrationDestinationManager::CollectionOptionsAndUUID
MigrationDestinationManager::getCollectionOptions(OperationContext* opCtx,
                                                  const NamespaceStringOrUUID& nssOrUUID,
                                                  const ShardId& fromShardId,
                                                  const boost::optional<DatabaseVersion>& dbVersion,
                                                  boost::optional<Timestamp> afterClusterTime) {
    auto fromShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, fromShardId));

    BSONObj fromOptions;

    auto cmd = nssOrUUID.isNamespaceString()
        ? BSON("listCollections" << 1 << "filter" << BSON("name" << nssOrUUID.nss().coll()))
        : BSON("listCollections" << 1 << "filter" << BSON("info.uuid" << nssOrUUID.uuid()));

    if (dbVersion) {
        cmd = appendDbVersionIfPresent(cmd, *dbVersion);
    }

    if (afterClusterTime) {
        cmd = cmd.addFields(makeLocalReadConcernWithAfterClusterTime(*afterClusterTime));
    }

    // Get collection options by calling listCollections against the from shard.
    auto infosRes = uassertStatusOK(
        fromShard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                              nssOrUUID.dbName().toString(),
                                              cmd,
                                              Milliseconds(-1)));

    auto infos = infosRes.docs;
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "expected listCollections against the primary shard for "
                          << nssOrUUID.toString() << " to return 1 entry, but got " << infos.size()
                          << " entries",
            infos.size() == 1);


    BSONObj entry = infos.front();

    // The entire options include both the settable options under the 'options' field in the
    // listCollections response, and the UUID under the 'info' field.
    BSONObjBuilder fromOptionsBob;

    if (entry["options"].isABSONObj()) {
        fromOptionsBob.appendElements(entry["options"].Obj());
    }

    BSONObj info;
    if (entry["info"].isABSONObj()) {
        info = entry["info"].Obj();
    }

    uassert(ErrorCodes::InvalidUUID,
            str::stream() << "The from shard did not return a UUID for collection "
                          << nssOrUUID.toString() << " as part of its listCollections response: "
                          << entry << ", but this node expects to see a UUID.",
            !info["uuid"].eoo());

    auto fromUUID = info["uuid"].uuid();

    fromOptionsBob.append(info["uuid"]);
    fromOptions = fromOptionsBob.obj();

    return {fromOptions, UUID::fromCDR(fromUUID)};
}

void MigrationDestinationManager::_dropLocalIndexesIfNecessary(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptionsAndIndexes& collectionOptionsAndIndexes) {
    bool dropNonDonorIndexes = [&]() -> bool {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        const auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
        // Only attempt to drop a collection's indexes if we have valid metadata and the collection
        // is sharded
        if (auto optMetadata = scopedCsr->getCurrentMetadataIfKnown()) {
            const auto& metadata = *optMetadata;
            if (metadata.isSharded()) {
                return !metadata.currentShardHasAnyChunks();
            }
        }
        return false;
    }();

    if (dropNonDonorIndexes) {
        // Determine which indexes exist on the local collection that don't exist on the donor's
        // collection.
        DBDirectClient client(opCtx);
        const bool includeBuildUUIDs = false;
        const int options = 0;
        auto indexes = client.getIndexSpecs(nss, includeBuildUUIDs, options);
        for (auto&& recipientIndex : indexes) {
            bool dropIndex = true;
            for (auto&& donorIndex : collectionOptionsAndIndexes.indexSpecs) {
                if (recipientIndex.woCompare(donorIndex) == 0) {
                    dropIndex = false;
                    break;
                }
            }
            // If the local index doesn't exist on the donor and isn't the _id index, drop it.
            auto indexNameElem = recipientIndex[IndexDescriptor::kIndexNameFieldName];
            if (indexNameElem.type() == BSONType::String && dropIndex &&
                !IndexDescriptor::isIdIndexPattern(
                    recipientIndex[IndexDescriptor::kKeyPatternFieldName].Obj())) {
                BSONObj info;
                if (!client.runCommand(
                        nss.dbName(),
                        BSON("dropIndexes" << nss.coll() << "index" << indexNameElem),
                        info))
                    uassertStatusOK(getStatusFromCommandResult(info));
            }
        }
    }
}

void MigrationDestinationManager::cloneCollectionIndexesAndOptions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptionsAndIndexes& collectionOptionsAndIndexes) {
    {
        // 1. Create the collection (if it doesn't already exist) and create any indexes we are
        // missing (auto-heal indexes).

        // Checks that the collection's UUID matches the donor's.
        auto checkUUIDsMatch = [&](const Collection* collection) {
            uassert(ErrorCodes::NotWritablePrimary,
                    str::stream() << "Unable to create collection " << nss.ns()
                                  << " because the node is not primary",
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

            uassert(ErrorCodes::InvalidUUID,
                    str::stream()
                        << "Cannot create collection " << nss.ns()
                        << " because we already have an identically named collection with UUID "
                        << collection->uuid() << ", which differs from the donor's UUID "
                        << collectionOptionsAndIndexes.uuid
                        << ". Manually drop the collection on this shard if it contains data from "
                           "a previous incarnation of "
                        << nss.ns(),
                    collection->uuid() == collectionOptionsAndIndexes.uuid);
        };

        bool isFirstMigration = [&] {
            AutoGetCollection collection(opCtx, nss, MODE_IS);
            const auto scopedCsr =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
            if (auto optMetadata = scopedCsr->getCurrentMetadataIfKnown()) {
                const auto& metadata = *optMetadata;
                return metadata.isSharded() && !metadata.currentShardHasAnyChunks();
            }
            return false;
        }();

        // Check if there are missing indexes on the recipient shard from the donor.
        // If it is the first migration, do not consider in-progress index builds. Otherwise,
        // consider in-progress index builds as ready. Then, if there are missing indexes and the
        // collection is not empty, fail the migration. On the other hand, if the collection is
        // empty, wait for index builds to finish if it is the first migration.
        bool waitForInProgressIndexBuildCompletion = false;

        auto checkEmptyOrGetMissingIndexesFromDonor = [&](const CollectionPtr& collection) {
            auto indexCatalog = collection->getIndexCatalog();
            auto indexSpecs = indexCatalog->removeExistingIndexesNoChecks(
                opCtx, collection, collectionOptionsAndIndexes.indexSpecs, !isFirstMigration);
            if (!indexSpecs.empty()) {
                // Only allow indexes to be copied if the collection does not have any documents.
                uassert(ErrorCodes::CannotCreateCollection,
                        str::stream()
                            << "aborting, shard is missing " << indexSpecs.size() << " indexes and "
                            << "collection is not empty. Non-trivial "
                            << "index creation should be scheduled manually",
                        collection->isEmpty(opCtx));

                // If it is the first migration, mark waitForInProgressIndexBuildCompletion as true
                // to wait for index builds to be finished after releasing the locks.
                waitForInProgressIndexBuildCompletion = isFirstMigration;
            }
            return indexSpecs;
        };

        {
            AutoGetCollection collection(opCtx, nss, MODE_IS);

            if (collection) {
                checkUUIDsMatch(collection.getCollection().get());
                auto indexSpecs =
                    checkEmptyOrGetMissingIndexesFromDonor(collection.getCollection());
                if (indexSpecs.empty()) {
                    return;
                }
            }
        }

        // Before taking the exclusive database lock for cloning the remaining indexes, wait for
        // index builds to finish if it is the first migration.
        if (waitForInProgressIndexBuildCompletion) {
            if (MONGO_unlikely(
                    hangMigrationRecipientBeforeWaitingNoIndexBuildInProgress.shouldFail())) {
                LOGV2(7677900, "Hanging before waiting for in-progress index builds to finish");
                hangMigrationRecipientBeforeWaitingNoIndexBuildInProgress.pauseWhileSet();
            }

            IndexBuildsCoordinator::get(opCtx)->awaitNoIndexBuildInProgressForCollection(
                opCtx, collectionOptionsAndIndexes.uuid);
        }

        // Acquire the exclusive collection lock to eventually create the collection and clone the
        // remaining indexes.
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);

        auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        if (collection) {
            checkUUIDsMatch(collection);
        } else {
            if (auto collectionByUUID = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(
                    opCtx, collectionOptionsAndIndexes.uuid)) {
                uasserted(5860300,
                          str::stream()
                              << "Cannot create collection " << nss << " with UUID "
                              << collectionOptionsAndIndexes.uuid
                              << " because it conflicts with the UUID of an existing collection "
                              << collectionByUUID->ns());
            }

            // We do not have a collection by this name. Create it with the donor's options.
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx, true /* forceCSRAsUnknownAfterCollectionCreation */);
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions collectionOptions = uassertStatusOK(
                CollectionOptions::parse(collectionOptionsAndIndexes.options,
                                         CollectionOptions::ParseKind::parseForStorage));
            uassertStatusOK(db->userCreateNS(opCtx,
                                             nss,
                                             collectionOptions,
                                             true /* createDefaultIndexes */,
                                             collectionOptionsAndIndexes.idIndexSpec,
                                             true /* fromMigrate */));
            wuow.commit();

            collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        }

        auto indexSpecs = checkEmptyOrGetMissingIndexesFromDonor(CollectionPtr(collection));
        if (!indexSpecs.empty()) {
            WriteUnitOfWork wunit(opCtx);
            CollectionWriter collWriter(opCtx, collection->uuid());
            IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                opCtx, collWriter, indexSpecs, true /* fromMigrate */);
            wunit.commit();
        }
    }
}

void MigrationDestinationManager::_migrateThread(CancellationToken cancellationToken,
                                                 bool skipToCritSecTaken) {
    invariant(_sessionId);

    Client::initThread("migrateThread");
    auto client = Client::getCurrent();
    {
        stdx::lock_guard lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    }

    bool recovering = false;
    while (true) {
        const auto executor =
            Grid::get(client->getServiceContext())->getExecutorPool()->getFixedExecutor();
        auto uniqueOpCtx =
            CancelableOperationContext(client->makeOperationContext(), cancellationToken, executor);
        auto opCtx = uniqueOpCtx.get();

        if (AuthorizationManager::get(opCtx->getServiceContext())->isAuthEnabled()) {
            AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization(opCtx);
        }

        try {
            if (recovering) {
                if (!migrationRecipientRecoveryDocumentExists(opCtx, *_sessionId)) {
                    // No need to run any recovery.
                    break;
                }
            }

            // The outer OperationContext is used to hold the session checked out for the
            // duration of the recipient's side of the migration. This guarantees that if the
            // donor shard has failed over, then the new donor primary cannot bump the
            // txnNumber on this session while this node is still executing the recipient side
            // (which is important because otherwise, this node may create orphans after the
            // range deletion task on this node has been processed). The recipient will periodically
            // yield this session, but will verify the txnNumber has not changed before continuing,
            // preserving the guarantee that orphans cannot be created after the txnNumber is
            // advanced.
            {
                auto lk = stdx::lock_guard(*opCtx->getClient());
                opCtx->setLogicalSessionId(_lsid);
                opCtx->setTxnNumber(_txnNumber);
            }

            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            auto sessionTxnState = mongoDSessionCatalog->checkOutSession(opCtx);

            auto txnParticipant = TransactionParticipant::get(opCtx);
            txnParticipant.beginOrContinue(opCtx,
                                           {*opCtx->getTxnNumber()},
                                           boost::none /* autocommit */,
                                           boost::none /* startTransaction */);
            _migrateDriver(opCtx, skipToCritSecTaken || recovering);
        } catch (...) {
            _setStateFail(str::stream() << "migrate failed: " << redact(exceptionToStatus()));

            if (!cancellationToken.isCanceled()) {
                // Run recovery if needed.
                recovering = true;
                continue;
            }
        }

        break;
    }

    stdx::lock_guard<Latch> lk(_mutex);
    _sessionId.reset();
    _scopedReceiveChunk.reset();
    _isActiveCV.notify_all();

    // If we reached this point without having set _canReleaseCriticalSectionPromise we must be on
    // an error path. Just set the promise with error because it is illegal to leave it unset on
    // destruction.
    invariant(_canReleaseCriticalSectionPromise);
    if (!_canReleaseCriticalSectionPromise->getFuture().isReady()) {
        _canReleaseCriticalSectionPromise->setError(
            {ErrorCodes::CallbackCanceled, "explicitly breaking release critical section promise"});
    }
    _canReleaseCriticalSectionPromise.reset();

    invariant(_migrateThreadFinishedPromise);
    _migrateThreadFinishedPromise->emplaceValue(_state);
    _migrateThreadFinishedPromise.reset();
}

void MigrationDestinationManager::_migrateDriver(OperationContext* outerOpCtx,
                                                 bool skipToCritSecTaken) {
    invariant(isActive());
    invariant(_sessionId);
    invariant(_scopedReceiveChunk);
    invariant(!_min.isEmpty());
    invariant(!_max.isEmpty());

    boost::optional<Timer> timeInCriticalSection;
    boost::optional<MoveTimingHelper> timing;
    mongo::ScopeGuard timingSetMsgGuard{[this, &timing] {
        // Set the error message to MoveTimingHelper just before it is destroyed. The destructor
        // sends that message (among other things) to the ShardingLogging.
        if (timing) {
            stdx::lock_guard<Latch> sl(_mutex);
            timing->setCmdErrMsg(_errmsg);
        }
    }};

    if (!skipToCritSecTaken) {
        timing.emplace(
            outerOpCtx, "to", _nss.ns(), _min, _max, 8 /* steps */, _toShard, _fromShard);

        LOGV2(
            22000,
            "Starting receiving end of migration of chunk {chunkMin} -> {chunkMax} for collection "
            "{namespace} from {fromShard} at epoch {epoch} with session id {sessionId}",
            "Starting receiving end of chunk migration",
            "chunkMin"_attr = redact(_min),
            "chunkMax"_attr = redact(_max),
            logAttrs(_nss),
            "fromShard"_attr = _fromShard,
            "epoch"_attr = _epoch,
            "sessionId"_attr = *_sessionId,
            "migrationId"_attr = _migrationId->toBSON());

        const auto initialState = getState();

        if (initialState == kAbort) {
            LOGV2_ERROR(22013,
                        "Migration abort requested before the migration started",
                        "migrationId"_attr = _migrationId->toBSON());
            return;
        }

        invariant(initialState == kReady);

        auto donorCollectionOptionsAndIndexes = [&]() -> CollectionOptionsAndIndexes {
            auto [collOptions, uuid] =
                getCollectionOptions(outerOpCtx, _nss, _fromShard, boost::none, boost::none);
            auto [indexes, idIndex] =
                getCollectionIndexes(outerOpCtx, _nss, _fromShard, boost::none, boost::none);
            return {uuid, indexes, idIndex, collOptions};
        }();

        _collectionUuid = donorCollectionOptionsAndIndexes.uuid;

        auto fromShard = uassertStatusOK(
            Grid::get(outerOpCtx)->shardRegistry()->getShard(outerOpCtx, _fromShard));

        const ChunkRange range(_min, _max);

        // 1. Ensure any data which might have been left orphaned in the range being moved has been
        // deleted.
        const auto rangeDeletionWaitDeadline =
            outerOpCtx->getServiceContext()->getFastClockSource()->now() +
            Milliseconds(drainOverlappingRangeDeletionsOnStartTimeoutMS.load());

        while (migrationutil::checkForConflictingDeletions(
            outerOpCtx, range, donorCollectionOptionsAndIndexes.uuid)) {
            uassert(ErrorCodes::ResumableRangeDeleterDisabled,
                    "Failing migration because the disableResumableRangeDeleter server "
                    "parameter is set to true on the recipient shard, which contains range "
                    "deletion tasks overlapping the incoming range.",
                    !disableResumableRangeDeleter.load());

            LOGV2(22001,
                  "Migration paused because the requested range {range} for {namespace} "
                  "overlaps with a range already scheduled for deletion",
                  "Migration paused because the requested range overlaps with a range already "
                  "scheduled for deletion",
                  logAttrs(_nss),
                  "range"_attr = redact(range.toString()),
                  "migrationId"_attr = _migrationId->toBSON());

            auto status =
                CollectionShardingRuntime::waitForClean(outerOpCtx,
                                                        _nss,
                                                        donorCollectionOptionsAndIndexes.uuid,
                                                        range,
                                                        rangeDeletionWaitDeadline);

            if (!status.isOK()) {
                _setStateFail(redact(status.toString()));
                return;
            }

            uassert(ErrorCodes::ExceededTimeLimit,
                    "Exceeded deadline waiting for overlapping range deletion to finish",
                    outerOpCtx->getServiceContext()->getFastClockSource()->now() <
                        rangeDeletionWaitDeadline);

            // If the filtering metadata was cleared while the range deletion task was ongoing, then
            // 'waitForClean' would return immediately even though there really is an ongoing range
            // deletion task. For that case, we loop again until there is no conflicting task in
            // config.rangeDeletions
            outerOpCtx->sleepFor(Milliseconds(1000));
        }

        timing->done(1);
        migrateThreadHangAtStep1.pauseWhileSet();


        // 2. Create the parent collection and its indexes, if needed.
        // The conventional usage of retryable writes is to assign statement id's to all of
        // the writes done as part of the data copying so that _recvChunkStart is
        // conceptually a retryable write batch. However, we are using an alternate approach to do
        // those writes under an AlternativeClientRegion because 1) threading the statement id's
        // through to all the places where they are needed would make this code more complex, and 2)
        // some of the operations, like creating the collection or building indexes, are not
        // currently supported in retryable writes.
        outerOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        {
            auto newClient = outerOpCtx->getServiceContext()->makeClient("MigrationCoordinator");
            {
                stdx::lock_guard<Client> lk(*newClient.get());
                newClient->setSystemOperationKillableByStepdown(lk);
            }

            AlternativeClientRegion acr(newClient);
            auto executor =
                Grid::get(outerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
            auto altOpCtx = CancelableOperationContext(
                cc().makeOperationContext(), outerOpCtx->getCancellationToken(), executor);

            // Enable write blocking bypass to allow migrations to create the collection and indexes
            // even when user writes are blocked.
            WriteBlockBypass::get(altOpCtx.get()).set(true);

            _dropLocalIndexesIfNecessary(altOpCtx.get(), _nss, donorCollectionOptionsAndIndexes);
            cloneCollectionIndexesAndOptions(
                altOpCtx.get(), _nss, donorCollectionOptionsAndIndexes);

            // Get the global indexes and install them.
            if (feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                replaceShardingIndexCatalogInShardIfNeeded(
                    altOpCtx.get(), _nss, donorCollectionOptionsAndIndexes.uuid);
            }

            timing->done(2);
            migrateThreadHangAtStep2.pauseWhileSet();
        }

        {
            // 3. Insert a pending range deletion task for the incoming range.
            RangeDeletionTask recipientDeletionTask(*_migrationId,
                                                    _nss,
                                                    donorCollectionOptionsAndIndexes.uuid,
                                                    _fromShard,
                                                    range,
                                                    CleanWhenEnum::kNow);
            recipientDeletionTask.setPending(true);
            const auto currentTime = VectorClock::get(outerOpCtx)->getTime();
            recipientDeletionTask.setTimestamp(currentTime.clusterTime().asTimestamp());
            recipientDeletionTask.setKeyPattern(KeyPattern(_shardKeyPattern));

            // It is illegal to wait for write concern with a session checked out, so persist the
            // range deletion task with an immediately satsifiable write concern and then wait for
            // majority after yielding the session.
            migrationutil::persistRangeDeletionTaskLocally(
                outerOpCtx, recipientDeletionTask, WriteConcernOptions());

            runWithoutSession(outerOpCtx, [&] {
                WriteConcernResult ignoreResult;
                auto latestOpTime =
                    repl::ReplClientInfo::forClient(outerOpCtx->getClient()).getLastOp();
                uassertStatusOK(
                    waitForWriteConcern(outerOpCtx,
                                        latestOpTime,
                                        WriteConcerns::kMajorityWriteConcernShardingTimeout,
                                        &ignoreResult));
            });

            timing->done(3);
            migrateThreadHangAtStep3.pauseWhileSet();
        }

        auto newClient = outerOpCtx->getServiceContext()->makeClient("MigrationCoordinator");
        {
            stdx::lock_guard<Client> lk(*newClient.get());
            newClient->setSystemOperationKillableByStepdown(lk);
        }
        AlternativeClientRegion acr(newClient);
        auto executor =
            Grid::get(outerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
        auto newOpCtxPtr = CancelableOperationContext(
            cc().makeOperationContext(), outerOpCtx->getCancellationToken(), executor);
        auto opCtx = newOpCtxPtr.get();
        repl::OpTime lastOpApplied;
        {
            // 4. Initial bulk clone
            _setState(kClone);

            _sessionMigration->start(opCtx->getServiceContext());

            _chunkMarkedPending = true;  // no lock needed, only the migrate thread looks.

            {
                // Destructor of MigrationBatchFetcher is non-trivial. Therefore,
                // this scope has semantic significance.
                MigrationBatchFetcher<MigrationBatchInserter> fetcher{
                    outerOpCtx,
                    opCtx,
                    _nss,
                    *_sessionId,
                    _writeConcern,
                    _fromShard,
                    range,
                    *_migrationId,
                    *_collectionUuid,
                    _migrationCloningProgress,
                    _parallelFetchersSupported,
                    chunkMigrationFetcherMaxBufferedSizeBytesPerThread.load()};
                fetcher.fetchAndScheduleInsertion();
            }
            opCtx->checkForInterrupt();
            lastOpApplied = _migrationCloningProgress->getMaxOptime();

            timing->done(4);
            migrateThreadHangAtStep4.pauseWhileSet();

            if (MONGO_unlikely(failMigrationOnRecipient.shouldFail())) {
                _setStateFail(str::stream() << "failing migration after cloning " << _getNumCloned()
                                            << " docs due to failMigrationOnRecipient failpoint");
                return;
            }
        }

        const BSONObj xferModsRequest = createTransferModsRequest(_nss, *_sessionId);

        {
            // 5. Do bulk of mods
            _setState(kCatchup);

            auto fetchBatchFn = [&](OperationContext* opCtx, BSONObj* nextBatch) {
                auto commandResponse = uassertStatusOKWithContext(
                    fromShard->runCommand(opCtx,
                                          ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                          "admin",
                                          xferModsRequest,
                                          Shard::RetryPolicy::kNoRetry),
                    "_transferMods failed: ");

                uassertStatusOKWithContext(
                    Shard::CommandResponse::getEffectiveStatus(commandResponse),
                    "_transferMods failed: ");

                *nextBatch = commandResponse.response;
                return nextBatch->getField("size").number() == 0;
            };

            auto applyModsFn = [&](OperationContext* opCtx, BSONObj nextBatch) {
                if (nextBatch["size"].number() == 0) {
                    // There are no more pending modifications to be applied. End the catchup phase
                    return false;
                }

                if (!_applyMigrateOp(opCtx, nextBatch)) {
                    return true;
                }
                ShardingStatistics::get(opCtx).countBytesClonedOnCatchUpOnRecipient.addAndFetch(
                    nextBatch["size"].number());

                const int maxIterations = 3600 * 50;

                int i;
                for (i = 0; i < maxIterations; i++) {
                    opCtx->checkForInterrupt();
                    outerOpCtx->checkForInterrupt();

                    uassert(
                        ErrorCodes::CommandFailed,
                        str::stream()
                            << "Migration aborted while waiting for replication at catch up stage, "
                            << _migrationId->toBSON(),
                        getState() != kAbort);

                    if (runWithoutSession(outerOpCtx, [&] {
                            return opReplicatedEnough(opCtx, lastOpApplied, _writeConcern);
                        })) {
                        return true;
                    }

                    if (i > 100) {
                        LOGV2(22003,
                              "secondaries having hard time keeping up with migrate",
                              "migrationId"_attr = _migrationId->toBSON());
                    }

                    sleepmillis(20);
                }

                uassert(ErrorCodes::CommandFailed,
                        "Secondary can't keep up with migrate",
                        i != maxIterations);

                return true;
            };

            auto updatedTime = fetchAndApplyBatch(opCtx, applyModsFn, fetchBatchFn);
            lastOpApplied = (updatedTime == repl::OpTime()) ? lastOpApplied : updatedTime;

            timing->done(5);
            migrateThreadHangAtStep5.pauseWhileSet();
        }

        {
            // Pause to wait for replication. This will prevent us from going into critical section
            // until we're ready.

            LOGV2(22004,
                  "Waiting for replication to catch up before entering critical section",
                  "migrationId"_attr = _migrationId->toBSON());
            LOGV2_DEBUG_OPTIONS(4817411,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Starting majority commit wait on recipient",
                                "migrationId"_attr = _migrationId->toBSON());

            runWithoutSession(outerOpCtx, [&] {
                auto awaitReplicationResult =
                    repl::ReplicationCoordinator::get(opCtx)->awaitReplication(
                        opCtx, lastOpApplied, WriteConcerns::kMajorityWriteConcernShardingTimeout);
                uassertStatusOKWithContext(awaitReplicationResult.status,
                                           awaitReplicationResult.status.codeString());
            });

            LOGV2(22005,
                  "Chunk data replicated successfully.",
                  "migrationId"_attr = _migrationId->toBSON());
            LOGV2_DEBUG_OPTIONS(4817412,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished majority commit wait on recipient",
                                "migrationId"_attr = _migrationId->toBSON());
        }

        {
            // 6. Wait for commit
            _setState(kSteady);
            migrateThreadHangAfterSteadyTransition.pauseWhileSet();

            bool transferAfterCommit = false;
            while (getState() == kSteady || getState() == kCommitStart) {
                opCtx->checkForInterrupt();
                outerOpCtx->checkForInterrupt();

                // Make sure we do at least one transfer after recv'ing the commit message. If we
                // aren't sure that at least one transfer happens *after* our state changes to
                // COMMIT_START, there could be mods still on the FROM shard that got logged
                // *after* our _transferMods but *before* the critical section.
                if (getState() == kCommitStart) {
                    transferAfterCommit = true;
                }

                auto res = uassertStatusOKWithContext(
                    fromShard->runCommand(opCtx,
                                          ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                          "admin",
                                          xferModsRequest,
                                          Shard::RetryPolicy::kNoRetry),
                    "_transferMods failed in STEADY STATE: ");

                uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(res),
                                           "_transferMods failed in STEADY STATE: ");

                auto mods = res.response;

                if (mods["size"].number() > 0) {
                    (void)_applyMigrateOp(opCtx, mods);
                    lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
                    continue;
                }

                if (getState() == kAbort) {
                    LOGV2(22006,
                          "Migration aborted while transferring mods",
                          "migrationId"_attr = _migrationId->toBSON());
                    return;
                }

                // We know we're finished when:
                // 1) The from side has told us that it has locked writes (COMMIT_START)
                // 2) We've checked at least one more time for un-transmitted mods
                if (getState() == kCommitStart && transferAfterCommit == true) {
                    if (runWithoutSession(outerOpCtx, [&] {
                            return _flushPendingWrites(opCtx, lastOpApplied);
                        })) {
                        break;
                    }
                }

                // Only sleep if we aren't committing
                if (getState() == kSteady)
                    sleepmillis(10);
            }

            if (getState() == kFail || getState() == kAbort) {
                _setStateFail("timed out waiting for commit");
                return;
            }

            timing->done(6);
            migrateThreadHangAtStep6.pauseWhileSet();
        }

        runWithoutSession(outerOpCtx, [&] { _sessionMigration->join(); });
        if (_sessionMigration->getState() ==
            SessionCatalogMigrationDestination::State::ErrorOccurred) {
            _setStateFail(redact(_sessionMigration->getErrMsg()));
            return;
        }

        timing->done(7);
        migrateThreadHangAtStep7.pauseWhileSet();

        const auto critSecReason = criticalSectionReason(*_sessionId);

        runWithoutSession(outerOpCtx, [&] {
            // Persist the migration recipient recovery document so that in case of failover, the
            // new primary will resume the MigrationDestinationManager and retake the critical
            // section.
            migrationutil::persistMigrationRecipientRecoveryDocument(
                opCtx, {*_migrationId, _nss, *_sessionId, range, _fromShard, _lsid, _txnNumber});

            LOGV2_DEBUG(5899113,
                        2,
                        "Persisted migration recipient recovery document",
                        "sessionId"_attr = _sessionId);

            // Enter critical section. Ensure it has been majority commited before _recvChunkCommit
            // returns success to the donor, so that if the recipient steps down, the critical
            // section is kept taken while the donor commits the migration.
            ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
                opCtx, _nss, critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

            LOGV2(5899114, "Entered migration recipient critical section", logAttrs(_nss));
            timeInCriticalSection.emplace();
        });

        if (getState() == kFail || getState() == kAbort) {
            _setStateFail("timed out waiting for critical section acquisition");
        }

        {
            // Make sure we don't overwrite a FAIL or ABORT state.
            stdx::lock_guard<Latch> sl(_mutex);
            if (_state != kFail && _state != kAbort) {
                _state = kEnteredCritSec;
                _stateChangedCV.notify_all();
            }
        }
    } else {
        outerOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        auto newClient = outerOpCtx->getServiceContext()->makeClient("MigrationCoordinator");
        {
            stdx::lock_guard<Client> lk(*newClient.get());
            newClient->setSystemOperationKillableByStepdown(lk);
        }
        AlternativeClientRegion acr(newClient);
        auto executor =
            Grid::get(outerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
        auto newOpCtxPtr = CancelableOperationContext(
            cc().makeOperationContext(), outerOpCtx->getCancellationToken(), executor);
        auto opCtx = newOpCtxPtr.get();

        ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
            opCtx,
            _nss,
            criticalSectionReason(*_sessionId),
            ShardingCatalogClient::kMajorityWriteConcern);

        LOGV2_DEBUG(6064501,
                    2,
                    "Reacquired migration recipient critical section",
                    "sessionId"_attr = *_sessionId);

        {
            stdx::lock_guard<Latch> sl(_mutex);
            _state = kEnteredCritSec;
            _stateChangedCV.notify_all();
        }

        LOGV2(6064503, "Recovered migration recipient", "sessionId"_attr = *_sessionId);
    }

    outerOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    auto newClient = outerOpCtx->getServiceContext()->makeClient("MigrationCoordinator");
    {
        stdx::lock_guard<Client> lk(*newClient.get());
        newClient->setSystemOperationKillableByStepdown(lk);
    }
    AlternativeClientRegion acr(newClient);
    auto executor =
        Grid::get(outerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
    auto newOpCtxPtr = CancelableOperationContext(
        cc().makeOperationContext(), outerOpCtx->getCancellationToken(), executor);
    auto opCtx = newOpCtxPtr.get();

    if (skipToCritSecTaken) {
        timeInCriticalSection.emplace();
    }
    invariant(timeInCriticalSection);

    // Wait until signaled to exit the critical section and then release it.
    runWithoutSession(outerOpCtx, [&] {
        awaitCriticalSectionReleaseSignalAndCompleteMigration(opCtx, *timeInCriticalSection);
    });

    _setState(kDone);

    if (timing) {
        timing->done(8);
    }
}

bool MigrationDestinationManager::_applyMigrateOp(OperationContext* opCtx, const BSONObj& xfer) {
    bool didAnything = false;
    long long changeInOrphans = 0;
    long long totalDocs = 0;

    // Deleted documents
    if (xfer["deleted"].isABSONObj()) {
        boost::optional<RemoveSaver> rs;
        if (serverGlobalParams.moveParanoia) {
            rs.emplace("moveChunk", _nss.ns().toString(), "removedDuring");
        }

        BSONObjIterator i(xfer["deleted"].Obj());
        while (i.more()) {
            totalDocs++;
            AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Collection " << _nss.ns()
                                  << " was dropped in the middle of the migration",
                    autoColl.getCollection());

            BSONObj id = i.next().Obj();

            // Do not apply delete if doc does not belong to the chunk being migrated
            BSONObj fullObj;
            if (Helpers::findById(opCtx, _nss, id, fullObj)) {
                if (!isInRange(fullObj, _min, _max, _shardKeyPattern)) {
                    if (MONGO_unlikely(failMigrationReceivedOutOfRangeOperation.shouldFail())) {
                        MONGO_UNREACHABLE;
                    }
                    continue;
                }
            }

            if (rs) {
                uassertStatusOK(rs->goingToDelete(fullObj));
            }

            writeConflictRetry(opCtx, "transferModsDeletes", _nss.ns(), [&] {
                deleteObjects(opCtx,
                              autoColl.getCollection(),
                              _nss,
                              id,
                              true /* justOne */,
                              false /* god */,
                              true /* fromMigrate */);
            });

            changeInOrphans--;
            didAnything = true;
        }
    }

    // Inserted or updated documents
    if (xfer["reload"].isABSONObj()) {
        BSONObjIterator i(xfer["reload"].Obj());
        while (i.more()) {
            totalDocs++;
            AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Collection " << _nss.ns()
                                  << " was dropped in the middle of the migration",
                    autoColl.getCollection());

            BSONObj updatedDoc = i.next().Obj();

            // do not apply insert/update if doc does not belong to the chunk being migrated
            if (!isInRange(updatedDoc, _min, _max, _shardKeyPattern)) {
                if (MONGO_unlikely(failMigrationReceivedOutOfRangeOperation.shouldFail())) {
                    MONGO_UNREACHABLE;
                }
                continue;
            }

            BSONObj localDoc;
            if (willOverrideLocalId(
                    opCtx, _nss, _min, _max, _shardKeyPattern, updatedDoc, &localDoc)) {
                // Exception will abort migration cleanly
                LOGV2_ERROR_OPTIONS(
                    16977,
                    {logv2::UserAssertAfterLog()},
                    "Cannot migrate chunk because the local document {localDoc} has the same _id "
                    "as the reloaded remote document {remoteDoc}",
                    "Cannot migrate chunk because the local document has the same _id as the "
                    "reloaded remote document",
                    "localDoc"_attr = redact(localDoc),
                    "remoteDoc"_attr = redact(updatedDoc),
                    "migrationId"_attr = _migrationId->toBSON());
            }

            // We are in write lock here, so sure we aren't killing
            writeConflictRetry(opCtx, "transferModsUpdates", _nss.ns(), [&] {
                auto res = Helpers::upsert(opCtx, _nss, updatedDoc, true);
                if (!res.upsertedId.isEmpty()) {
                    changeInOrphans++;
                }
            });

            didAnything = true;
        }
    }

    if (changeInOrphans != 0) {
        persistUpdatedNumOrphans(opCtx, *_collectionUuid, ChunkRange(_min, _max), changeInOrphans);
    }

    ShardingStatistics::get(opCtx).countDocsClonedOnCatchUpOnRecipient.addAndFetch(totalDocs);

    return didAnything;
}

bool MigrationDestinationManager::_flushPendingWrites(OperationContext* opCtx,
                                                      const repl::OpTime& lastOpApplied) {
    if (!opReplicatedEnough(opCtx, lastOpApplied, _writeConcern)) {
        repl::OpTime op(lastOpApplied);
        static Occasionally sampler;
        if (sampler.tick()) {
            LOGV2(22007,
                  "Migration commit waiting for majority replication for {namespace}, "
                  "{chunkMin} -> {chunkMax}; waiting to reach this operation: {lastOpApplied}",
                  "Migration commit waiting for majority replication; waiting until the last "
                  "operation applied has been replicated",
                  logAttrs(_nss),
                  "chunkMin"_attr = redact(_min),
                  "chunkMax"_attr = redact(_max),
                  "lastOpApplied"_attr = op,
                  "migrationId"_attr = _migrationId->toBSON());
        }
        return false;
    }

    LOGV2(22008,
          "Migration commit succeeded flushing to secondaries for {namespace}, {min} -> {max}",
          "Migration commit succeeded flushing to secondaries",
          logAttrs(_nss),
          "chunkMin"_attr = redact(_min),
          "chunkMax"_attr = redact(_max),
          "migrationId"_attr = _migrationId->toBSON());

    return true;
}

void MigrationDestinationManager::awaitCriticalSectionReleaseSignalAndCompleteMigration(
    OperationContext* opCtx, const Timer& timeInCriticalSection) {
    // Wait until the migrate thread is signaled to release the critical section
    LOGV2_DEBUG(5899111, 3, "Waiting for release critical section signal");
    invariant(_canReleaseCriticalSectionPromise);
    _canReleaseCriticalSectionPromise->getFuture().get(opCtx);

    _setState(kExitCritSec);

    // Refresh the filtering metadata
    LOGV2_DEBUG(5899112, 3, "Refreshing filtering metadata before exiting critical section");

    bool refreshFailed = false;
    try {
        if (MONGO_unlikely(migrationRecipientFailPostCommitRefresh.shouldFail())) {
            uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
        }

        forceShardFilteringMetadataRefresh(opCtx, _nss);
    } catch (const DBException& ex) {
        LOGV2_DEBUG(5899103,
                    2,
                    "Post-migration commit refresh failed on recipient",
                    "migrationId"_attr = _migrationId,
                    "error"_attr = redact(ex));
        refreshFailed = true;
    }

    if (refreshFailed) {
        AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, _nss)
            ->clearFilteringMetadata(opCtx);
    }

    // Release the critical section
    LOGV2_DEBUG(5899110, 3, "Exiting critical section");
    const auto critSecReason = criticalSectionReason(*_sessionId);

    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx, _nss, critSecReason, ShardingCatalogClient::kMajorityWriteConcern);

    const auto timeInCriticalSectionMs = timeInCriticalSection.millis();
    ShardingStatistics::get(opCtx).totalRecipientCriticalSectionTimeMillis.addAndFetch(
        timeInCriticalSectionMs);

    LOGV2(5899108,
          "Exited migration recipient critical section",
          logAttrs(_nss),
          "durationMillis"_attr = timeInCriticalSectionMs);

    // Wait for the updates to the catalog cache to be written to disk before removing the
    // recovery document. This ensures that on case of stepdown, the new primary will know of a
    // placement version inclusive of the migration. NOTE: We rely on the
    // deleteMigrationRecipientRecoveryDocument call below to wait for the CatalogCache on-disk
    // persistence to be majority committed.
    CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, _nss);

    // Delete the recovery document
    migrationutil::deleteMigrationRecipientRecoveryDocument(opCtx, *_migrationId);
}

void MigrationDestinationManager::onStepUpBegin(OperationContext* opCtx, long long term) {
    stdx::lock_guard<Latch> sl(_mutex);
    auto newCancellationSource = CancellationSource();
    std::swap(_cancellationSource, newCancellationSource);
}

void MigrationDestinationManager::onStepDown() {
    boost::optional<SharedSemiFuture<State>> migrateThreadFinishedFuture;
    {
        stdx::lock_guard<Latch> sl(_mutex);
        // Cancel any migrateThread work.
        _cancellationSource.cancel();

        if (_migrateThreadFinishedPromise) {
            migrateThreadFinishedFuture = _migrateThreadFinishedPromise->getFuture();
        }
    }

    // Wait for the migrateThread to finish.
    if (migrateThreadFinishedFuture) {
        migrateThreadFinishedFuture->wait();
    }
}

}  // namespace mongo
