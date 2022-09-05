/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/s/migration_util.h"

#include <fmt/format.h>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/ensure_chunk_version_is_greater_than_gen.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

namespace mongo {
namespace migrationutil {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangBeforeFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumThenSimulateErrorUninterruptible);

const char kSourceShard[] = "source";
const char kDestinationShard[] = "destination";
const char kIsDonorShard[] = "isDonorShard";
const char kChunk[] = "chunk";
const char kCollection[] = "collection";
const auto kLogRetryAttemptThreshold = 20;
const Backoff kExponentialBackoff(Seconds(10), Milliseconds::max());

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);

class MigrationUtilExecutor {
public:
    MigrationUtilExecutor()
        : _executor(std::make_shared<executor::ThreadPoolTaskExecutor>(
              _makePool(), executor::makeNetworkInterface("MigrationUtil-TaskExecutor"))) {}

    void shutDownAndJoin() {
        _executor->shutdown();
        _executor->join();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> getExecutor() {
        stdx::lock_guard<Latch> lg(_mutex);
        if (!_started) {
            _executor->startup();
            _started = true;
        }
        return _executor;
    }

private:
    std::unique_ptr<ThreadPool> _makePool() {
        ThreadPool::Options options;
        options.poolName = "MoveChunk";
        options.minThreads = 0;
        options.maxThreads = 16;
        return std::make_unique<ThreadPool>(std::move(options));
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;

    // TODO SERVER-57253: get rid of _mutex and _started fields
    Mutex _mutex = MONGO_MAKE_LATCH("MigrationUtilExecutor::_mutex");
    bool _started = false;
};

const auto migrationUtilExecutorDecoration =
    ServiceContext::declareDecoration<MigrationUtilExecutor>();
const ServiceContext::ConstructorActionRegisterer migrationUtilExecutorRegisterer{
    "MigrationUtilExecutor",
    [](ServiceContext* service) {
        // TODO SERVER-57253: start migration util executor at decoration construction time
    },
    [](ServiceContext* service) { migrationUtilExecutorDecoration(service).shutDownAndJoin(); }};

template <typename Cmd>
void sendWriteCommandToRecipient(OperationContext* opCtx,
                                 const ShardId& recipientId,
                                 const Cmd& cmd,
                                 const BSONObj& passthroughFields = {}) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientId));

    auto cmdBSON = cmd.toBSON(passthroughFields);
    LOGV2_DEBUG(22023, 1, "Sending request to recipient", "commandToSend"_attr = redact(cmdBSON));

    auto response = recipientShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        cmd.getDbName().toString(),
        cmdBSON,
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(response.getStatus());
    uassertStatusOK(getStatusFromWriteCommandReply(response.getValue().response));
}

/**
 * Runs doWork until it doesn't throw an error, the node is shutting down, the node has stepped
 * down, or the node has stepped down and up.
 *
 * Note that it is not guaranteed that 'doWork' will not be executed while the node is secondary
 * or after the node has stepped down and up, only that 'doWork' will eventually stop being retried
 * if one of those events has happened.
 *
 * Requirements:
 * - doWork must be idempotent.
 */
void retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
    OperationContext* opCtx,
    StringData taskDescription,
    std::function<void(OperationContext*)> doWork,
    boost::optional<Backoff> backoff = boost::none) {
    const std::string newClientName = "{}-{}"_format(getThreadName(), taskDescription);
    const auto initialTerm = repl::ReplicationCoordinator::get(opCtx)->getTerm();

    for (int attempt = 1;; attempt++) {
        // Since we can't differenciate if a shutdown exception is coming from a remote node or
        // locally we need to directly inspect the the global shutdown state to correctly interrupt
        // this task in case this node is shutting down.
        if (globalInShutdownDeprecated()) {
            uasserted(ErrorCodes::ShutdownInProgress, "Shutdown in progress");
        }

        // If the node is no longer primary, stop retrying.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                "Stepped down while {}"_format(taskDescription),
                repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                    repl::MemberState::RS_PRIMARY);

        // If the term changed, that means that the step up recovery could have run or is running
        // so stop retrying in order to avoid duplicate work.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                "Term changed while {}"_format(taskDescription),
                initialTerm == repl::ReplicationCoordinator::get(opCtx)->getTerm());

        try {
            auto newClient = opCtx->getServiceContext()->makeClient(newClientName);

            {
                stdx::lock_guard<Client> lk(*newClient.get());
                newClient->setSystemOperationKillableByStepdown(lk);
            }

            auto newOpCtx = newClient->makeOperationContext();
            AlternativeClientRegion altClient(newClient);

            doWork(newOpCtx.get());
            break;
        } catch (DBException& ex) {
            if (backoff) {
                sleepFor(backoff->nextSleep());
            }

            if (attempt % kLogRetryAttemptThreshold == 1) {
                LOGV2_WARNING(23937,
                              "Retrying task after failed attempt",
                              "taskDescription"_attr = redact(taskDescription),
                              "attempt"_attr = attempt,
                              "error"_attr = redact(ex));
            }
        }
    }
}

void refreshFilteringMetadataUntilSuccess(OperationContext* opCtx, const NamespaceString& nss) {
    hangBeforeFilteringMetadataRefresh.pauseWhileSet();

    retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
        opCtx, "refreshFilteringMetadataUntilSuccess", [&nss](OperationContext* newOpCtx) {
            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(newOpCtx);

            try {
                onShardVersionMismatch(newOpCtx, nss, boost::none);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // Can throw NamespaceNotFound if the collection/database was dropped
            }

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for onShardVersionMismatch");
            }
        });
}

void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& collUUID,
                                     const ChunkRange& range,
                                     const ChunkVersion& preMigrationChunkVersion) {
    ConfigsvrEnsureChunkVersionIsGreaterThan ensureChunkVersionIsGreaterThanRequest;
    ensureChunkVersionIsGreaterThanRequest.setDbName(NamespaceString::kAdminDb);
    ensureChunkVersionIsGreaterThanRequest.setMinKey(range.getMin());
    ensureChunkVersionIsGreaterThanRequest.setMaxKey(range.getMax());
    ensureChunkVersionIsGreaterThanRequest.setVersion(preMigrationChunkVersion);
    ensureChunkVersionIsGreaterThanRequest.setNss(nss);
    ensureChunkVersionIsGreaterThanRequest.setCollectionUUID(collUUID);
    const auto ensureChunkVersionIsGreaterThanRequestBSON =
        ensureChunkVersionIsGreaterThanRequest.toBSON({});

    hangInEnsureChunkVersionIsGreaterThanInterruptible.pauseWhileSet(opCtx);

    const auto ensureChunkVersionIsGreaterThanResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            CommandHelpers::appendMajorityWriteConcern(ensureChunkVersionIsGreaterThanRequestBSON),
            Shard::RetryPolicy::kIdempotent);
    const auto ensureChunkVersionIsGreaterThanStatus =
        Shard::CommandResponse::getEffectiveStatus(ensureChunkVersionIsGreaterThanResponse);

    uassertStatusOK(ensureChunkVersionIsGreaterThanStatus);

    if (hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible.shouldFail()) {
        hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible.pauseWhileSet();
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response for _configsvrEnsureChunkVersionIsGreaterThan");
    }
}

}  // namespace

std::shared_ptr<executor::ThreadPoolTaskExecutor> getMigrationUtilExecutor(
    ServiceContext* serviceContext) {
    return migrationUtilExecutorDecoration(serviceContext).getExecutor();
}

BSONObj makeMigrationStatusDocument(const NamespaceString& nss,
                                    const ShardId& fromShard,
                                    const ShardId& toShard,
                                    const bool& isDonorShard,
                                    const BSONObj& min,
                                    const BSONObj& max) {
    BSONObjBuilder builder;
    builder.append(kSourceShard, fromShard.toString());
    builder.append(kDestinationShard, toShard.toString());
    builder.append(kIsDonorShard, isDonorShard);
    builder.append(kChunk, BSON(ChunkType::min(min) << ChunkType::max(max)));
    builder.append(kCollection, nss.ns());
    return builder.obj();
}

ChunkRange extendOrTruncateBoundsForMetadata(const CollectionMetadata& metadata,
                                             const ChunkRange& range) {
    auto metadataShardKeyPattern = KeyPattern(metadata.getKeyPattern());

    // If the input range is shorter than the range in the ChunkManager inside
    // 'metadata', we must extend its bounds to get a correct comparison. If the input
    // range is longer than the range in the ChunkManager, we likewise must shorten it.
    // We make sure to match what's in the ChunkManager instead of the other way around,
    // since the ChunkManager only stores ranges and compares overlaps using a string version of the
    // key, rather than a BSONObj. This logic is necessary because the _metadata list can
    // contain ChunkManagers with different shard keys if the shard key has been refined.
    //
    // Note that it's safe to use BSONObj::nFields() (which returns the number of top level
    // fields in the BSONObj) to compare the two, since shard key refine operations can only add
    // top-level fields.
    //
    // Using extractFieldsUndotted to shorten the input range is correct because the ChunkRange and
    // the shard key pattern will both already store nested shard key fields as top-level dotted
    // fields, and extractFieldsUndotted uses the top-level fields verbatim rather than treating
    // dots as accessors for subfields.
    auto metadataShardKeyPatternBson = metadataShardKeyPattern.toBSON();
    auto numFieldsInMetadataShardKey = metadataShardKeyPatternBson.nFields();
    auto numFieldsInInputRangeShardKey = range.getMin().nFields();
    if (numFieldsInInputRangeShardKey < numFieldsInMetadataShardKey) {
        auto extendedRangeMin = metadataShardKeyPattern.extendRangeBound(
            range.getMin(), false /* makeUpperInclusive */);
        auto extendedRangeMax = metadataShardKeyPattern.extendRangeBound(
            range.getMax(), false /* makeUpperInclusive */);
        return ChunkRange(extendedRangeMin, extendedRangeMax);
    } else if (numFieldsInInputRangeShardKey > numFieldsInMetadataShardKey) {
        auto shortenedRangeMin = range.getMin().extractFieldsUndotted(metadataShardKeyPatternBson);
        auto shortenedRangeMax = range.getMax().extractFieldsUndotted(metadataShardKeyPatternBson);
        return ChunkRange(shortenedRangeMin, shortenedRangeMax);
    } else {
        return range;
    }
}

BSONObj overlappingRangeQuery(const ChunkRange& range, const UUID& uuid) {
    return BSON(RangeDeletionTask::kCollectionUuidFieldName
                << uuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey << LT
                << range.getMax() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
                << GT << range.getMin());
}

size_t checkForConflictingDeletions(OperationContext* opCtx,
                                    const ChunkRange& range,
                                    const UUID& uuid) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    return store.count(opCtx, overlappingRangeQuery(range, uuid));
}

bool deletionTaskUuidMatchesFilteringMetadataUuid(
    OperationContext* opCtx,
    const boost::optional<mongo::CollectionMetadata>& optCollDescr,
    const RangeDeletionTask& deletionTask) {
    return optCollDescr && optCollDescr->isSharded() &&
        optCollDescr->uuidMatches(deletionTask.getCollectionUuid());
}

ExecutorFuture<void> cleanUpRange(ServiceContext* serviceContext,
                                  const std::shared_ptr<executor::ThreadPoolTaskExecutor>& executor,
                                  const RangeDeletionTask& deletionTask) {
    return AsyncTry([=]() mutable {
               ThreadClient tc(kRangeDeletionThreadName, serviceContext);
               {
                   stdx::lock_guard<Client> lk(*tc.get());
                   tc->setSystemOperationKillableByStepdown(lk);
               }
               auto uniqueOpCtx = tc->makeOperationContext();
               auto opCtx = uniqueOpCtx.get();
               opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

               const NamespaceString& nss = deletionTask.getNss();

               while (true) {
                   {
                       // Holding the locks while enqueueing the task protects against possible
                       // concurrent cleanups of the filtering metadata, that be serialized
                       AutoGetCollection autoColl(opCtx, nss, MODE_IS);
                       auto csr = CollectionShardingRuntime::get(opCtx, nss);
                       auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
                       auto optCollDescr = csr->getCurrentMetadataIfKnown();

                       if (optCollDescr) {
                           uassert(ErrorCodes::
                                       RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist,
                                   str::stream() << "Filtering metadata for " << nss
                                                 << (optCollDescr->isSharded()
                                                         ? " has UUID that does not match UUID of "
                                                           "the deletion task"
                                                         : " is unsharded"),
                                   deletionTaskUuidMatchesFilteringMetadataUuid(
                                       opCtx, optCollDescr, deletionTask));

                           LOGV2(22026,
                                 "Submitting range deletion task",
                                 "deletionTask"_attr = redact(deletionTask.toBSON()),
                                 "migrationId"_attr = deletionTask.getId());

                           const auto whenToClean =
                               deletionTask.getWhenToClean() == CleanWhenEnum::kNow
                               ? CollectionShardingRuntime::kNow
                               : CollectionShardingRuntime::kDelayed;

                           return csr->cleanUpRange(
                               deletionTask.getRange(), deletionTask.getId(), whenToClean);
                       }
                   }

                   refreshFilteringMetadataUntilSuccess(opCtx, nss);
               }
           })
        .until([](Status status) mutable {
            // Resubmit the range for deletion on a RangeOverlapConflict error.
            return status != ErrorCodes::RangeOverlapConflict;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(executor, CancellationToken::uncancelable());
}

ExecutorFuture<void> submitRangeDeletionTask(OperationContext* opCtx,
                                             const RangeDeletionTask& deletionTask) {
    const auto serviceContext = opCtx->getServiceContext();
    auto executor = getMigrationUtilExecutor(serviceContext);
    return ExecutorFuture<void>(executor)
        .then([=] {
            ThreadClient tc(kRangeDeletionThreadName, serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            uassert(
                ErrorCodes::ResumableRangeDeleterDisabled,
                str::stream()
                    << "Not submitting range deletion task " << redact(deletionTask.toBSON())
                    << " because the disableResumableRangeDeleter server parameter is set to true",
                !disableResumableRangeDeleter.load());

            return AsyncTry([=]() {
                       return cleanUpRange(serviceContext, executor, deletionTask)
                           .onError<ErrorCodes::KeyPatternShorterThanBound>([=](Status status) {
                               ThreadClient tc(kRangeDeletionThreadName, serviceContext);
                               {
                                   stdx::lock_guard<Client> lk(*tc.get());
                                   tc->setSystemOperationKillableByStepdown(lk);
                               }
                               auto uniqueOpCtx = tc->makeOperationContext();
                               uniqueOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                               LOGV2(55557,
                                     "cleanUpRange failed due to keyPattern shorter than range "
                                     "deletion bounds. Refreshing collection metadata to retry.",
                                     logAttrs(deletionTask.getNss()),
                                     "status"_attr = redact(status));

                               onShardVersionMismatch(
                                   uniqueOpCtx.get(), deletionTask.getNss(), boost::none);

                               return status;
                           });
                   })
                .until(
                    [](Status status) { return status != ErrorCodes::KeyPatternShorterThanBound; })
                .on(executor, CancellationToken::uncancelable());
        })
        .onError([=](const Status status) {
            ThreadClient tc(kRangeDeletionThreadName, serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            LOGV2(22027,
                  "Failed to submit range deletion task",
                  "deletionTask"_attr = redact(deletionTask.toBSON()),
                  "error"_attr = redact(status),
                  "migrationId"_attr = deletionTask.getId());

            if (status == ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
                deleteRangeDeletionTaskLocally(
                    opCtx, deletionTask.getId(), ShardingCatalogClient::kLocalWriteConcern);
            }

            // Note, we use onError and make it return its input status, because ExecutorFuture does
            // not support tapError.
            return status;
        });
}

void submitPendingDeletions(OperationContext* opCtx) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    auto query = BSON("pending" << BSON("$exists" << false));

    store.forEach(opCtx, query, [&opCtx](const RangeDeletionTask& deletionTask) {
        migrationutil::submitRangeDeletionTask(opCtx, deletionTask).getAsync([](auto) {});
        return true;
    });
}

void resubmitRangeDeletionsOnStepUp(ServiceContext* serviceContext) {
    LOGV2(22028, "Starting pending deletion submission thread.");

    ExecutorFuture<void>(getMigrationUtilExecutor(serviceContext))
        .then([serviceContext] {
            ThreadClient tc("ResubmitRangeDeletions", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            auto opCtx = tc->makeOperationContext();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            DBDirectClient client(opCtx.get());
            FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
            findCommand.setFilter(BSON(RangeDeletionTask::kProcessingFieldName << true));
            auto cursor = client.find(std::move(findCommand));

            auto retFuture = ExecutorFuture<void>(getMigrationUtilExecutor(serviceContext));

            int rangeDeletionsMarkedAsProcessing = 0;
            while (cursor->more()) {
                retFuture = migrationutil::submitRangeDeletionTask(
                    opCtx.get(),
                    RangeDeletionTask::parse(IDLParserContext("rangeDeletionRecovery"),
                                             cursor->next()));
                rangeDeletionsMarkedAsProcessing++;
            }

            if (rangeDeletionsMarkedAsProcessing > 1) {
                LOGV2_WARNING(
                    6695800,
                    "Rescheduling several range deletions marked as processing. Orphans count "
                    "may be off while they are not drained",
                    "numRangeDeletionsMarkedAsProcessing"_attr = rangeDeletionsMarkedAsProcessing);
            }

            return retFuture;
        })
        .then([serviceContext] {
            ThreadClient tc("ResubmitRangeDeletions", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            auto opCtx = tc->makeOperationContext();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            submitPendingDeletions(opCtx.get());
        })
        .getAsync([](auto) {});
}

void persistMigrationCoordinatorLocally(OperationContext* opCtx,
                                        const MigrationCoordinatorDocument& migrationDoc) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    try {
        store.add(opCtx, migrationDoc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(
            31374,
            str::stream() << "While attempting to write migration information for migration "
                          << ", found document with the same migration id. Attempted migration: "
                          << migrationDoc.toBSON());
    }
}

void persistRangeDeletionTaskLocally(OperationContext* opCtx,
                                     const RangeDeletionTask& deletionTask,
                                     const WriteConcernOptions& writeConcern) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    try {
        store.add(opCtx, deletionTask, writeConcern);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(31375,
                  str::stream() << "While attempting to write range deletion task for migration "
                                << ", found document with the same migration id. Attempted range "
                                   "deletion task: "
                                << deletionTask.toBSON());
    }
}

void persistUpdatedNumOrphans(OperationContext* opCtx,
                              const UUID& migrationId,
                              const UUID& collectionUuid,
                              long long changeInOrphans) {
    BSONObj query = BSON("_id" << migrationId);
    try {
        PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
        ScopedRangeDeleterLock rangeDeleterLock(opCtx, collectionUuid);
        // The DBDirectClient will not retry WriteConflictExceptions internally while holding an X
        // mode lock, so we need to retry at this level.
        writeConflictRetry(
            opCtx, "updateOrphanCount", NamespaceString::kRangeDeletionNamespace.ns(), [&] {
                store.update(opCtx,
                             query,
                             BSON("$inc" << BSON(RangeDeletionTask::kNumOrphanDocsFieldName
                                                 << changeInOrphans)),
                             WriteConcerns::kLocalWriteConcern);
            });
        BalancerStatsRegistry::get(opCtx)->updateOrphansCount(collectionUuid, changeInOrphans);
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        // When upgrading or downgrading, there may be no documents with the orphan count field.
    }
}

long long retrieveNumOrphansFromRecipient(OperationContext* opCtx,
                                          const MigrationCoordinatorDocument& migrationInfo) {
    const auto recipientShard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, migrationInfo.getRecipientShardId()));
    FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
    findCommand.setFilter(BSON("_id" << migrationInfo.getId()));
    findCommand.setReadConcern(BSONObj());
    Shard::QueryResponse rangeDeletionResponse =
        uassertStatusOK(recipientShard->runExhaustiveCursorCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            NamespaceString::kRangeDeletionNamespace.db().toString(),
            findCommand.toBSON(BSONObj()),
            Milliseconds(-1)));
    if (rangeDeletionResponse.docs.empty()) {
        // In case of shutdown/stepdown, the recipient may have already deleted its range deletion
        // document. A previous call to this function will have already returned the correct number
        // of orphans, so we can simply return 0.
        LOGV2_DEBUG(6376301,
                    2,
                    "No matching document found for migration",
                    "recipientId"_attr = migrationInfo.getRecipientShardId(),
                    "migrationId"_attr = migrationInfo.getId());
        return 0;
    }
    const auto numOrphanDocsElem =
        rangeDeletionResponse.docs[0].getField(RangeDeletionTask::kNumOrphanDocsFieldName);
    return numOrphanDocsElem.safeNumberLong();
}

void notifyChangeStreamsOnRecipientFirstChunk(OperationContext* opCtx,
                                              const NamespaceString& collNss,
                                              const ShardId& fromShardId,
                                              const ShardId& toShardId,
                                              boost::optional<UUID> collUUID) {

    const std::string dbgMessage = str::stream()
        << "Migrating chunk from shard " << fromShardId << " to shard " << toShardId
        << " with no chunks for this collection";

    // The message expected by change streams
    const auto o2Message =
        BSON("migrateChunkToNewShard" << collNss.toString() << "fromShardId" << fromShardId
                                      << "toShardId" << toShardId);

    auto const serviceContext = opCtx->getClient()->getServiceContext();

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
    writeConflictRetry(
        opCtx, "migrateChunkToNewShard", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork uow(opCtx);
            serviceContext->getOpObserver()->onInternalOpMessage(opCtx,
                                                                 collNss,
                                                                 *collUUID,
                                                                 BSON("msg" << dbgMessage),
                                                                 o2Message,
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none);
            uow.commit();
        });
}

void notifyChangeStreamsOnDonorLastChunk(OperationContext* opCtx,
                                         const NamespaceString& collNss,
                                         const ShardId& donorShardId,
                                         boost::optional<UUID> collUUID) {

    const std::string oMessage = str::stream()
        << "Migrate the last chunk for " << collNss << " off shard " << donorShardId;

    // The message expected by change streams
    const auto o2Message =
        BSON("migrateLastChunkFromShard" << collNss.toString() << "shardId" << donorShardId);

    auto const serviceContext = opCtx->getClient()->getServiceContext();

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
    writeConflictRetry(
        opCtx, "migrateLastChunkFromShard", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork uow(opCtx);
            serviceContext->getOpObserver()->onInternalOpMessage(opCtx,
                                                                 collNss,
                                                                 *collUUID,
                                                                 BSON("msg" << oMessage),
                                                                 o2Message,
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none);
            uow.commit();
        });
}

void persistCommitDecision(OperationContext* opCtx,
                           const MigrationCoordinatorDocument& migrationDoc) {
    invariant(migrationDoc.getDecision() &&
              *migrationDoc.getDecision() == DecisionEnum::kCommitted);

    hangInPersistMigrateCommitDecisionInterruptible.pauseWhileSet(opCtx);
    try {
        PersistentTaskStore<MigrationCoordinatorDocument> store(
            NamespaceString::kMigrationCoordinatorsNamespace);
        store.update(opCtx,
                     BSON(MigrationCoordinatorDocument::kIdFieldName << migrationDoc.getId()),
                     migrationDoc.toBSON());
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        LOGV2_ERROR(6439800,
                    "No coordination doc found on disk for migration",
                    "migration"_attr = redact(migrationDoc.toBSON()));
    }

    if (hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.shouldFail()) {
        hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when persisting migrate commit decision");
    }
}

void persistAbortDecision(OperationContext* opCtx,
                          const MigrationCoordinatorDocument& migrationDoc) {
    invariant(migrationDoc.getDecision() && *migrationDoc.getDecision() == DecisionEnum::kAborted);

    try {
        PersistentTaskStore<MigrationCoordinatorDocument> store(
            NamespaceString::kMigrationCoordinatorsNamespace);
        store.update(opCtx,
                     BSON(MigrationCoordinatorDocument::kIdFieldName << migrationDoc.getId()),
                     migrationDoc.toBSON());
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        LOGV2(6439801,
              "No coordination doc found on disk for migration",
              "migration"_attr = redact(migrationDoc.toBSON()));
    }

    if (hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.shouldFail()) {
        hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when persisting migrate abort decision");
    }
}

void deleteRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                        const ShardId& recipientId,
                                        const UUID& migrationId) {
    write_ops::DeleteCommandRequest deleteOp(NamespaceString::kRangeDeletionNamespace);
    write_ops::DeleteOpEntry query(BSON(RangeDeletionTask::kIdFieldName << migrationId),
                                   false /*multi*/);
    deleteOp.setDeletes({query});

    hangInDeleteRangeDeletionOnRecipientInterruptible.pauseWhileSet(opCtx);

    sendWriteCommandToRecipient(
        opCtx,
        recipientId,
        deleteOp,
        BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));

    if (hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
        hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when deleting range deletion on recipient");
    }
}

void deleteRangeDeletionTaskLocally(OperationContext* opCtx,
                                    const UUID& deletionTaskId,
                                    const WriteConcernOptions& writeConcern) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.remove(opCtx, BSON(RangeDeletionTask::kIdFieldName << deletionTaskId), writeConcern);

    if (hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
        hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when deleting range deletion locally");
    }
}

void markAsReadyRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                             const ShardId& recipientId,
                                             const UUID& migrationId) {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kRangeDeletionNamespace);
    auto queryFilter = BSON(RangeDeletionTask::kIdFieldName << migrationId);
    auto updateModification =
        write_ops::UpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << ""))));
    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(false);
    updateOp.setUpdates({updateEntry});

    retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
        opCtx, "ready remote range deletion", [&](OperationContext* newOpCtx) {
            try {
                sendWriteCommandToRecipient(
                    newOpCtx,
                    recipientId,
                    updateOp,
                    BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
            } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& exShardNotFound) {
                LOGV2_DEBUG(4620232,
                            1,
                            "Failed to mark range deletion task on recipient shard as ready",
                            "migrationId"_attr = migrationId,
                            "error"_attr = exShardNotFound);
                return;
            }

            if (hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
                hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when initiating range deletion on recipient");
            }
        });
}

void advanceTransactionOnRecipient(OperationContext* opCtx,
                                   const ShardId& recipientId,
                                   const LogicalSessionId& lsid,
                                   TxnNumber currentTxnNumber) {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
    auto queryFilter = BSON("_id"
                            << "migrationCoordinatorStats");
    auto updateModification = write_ops::UpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$inc" << BSON("count" << 1))));

    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(true);
    updateOp.setUpdates({updateEntry});

    auto passthroughFields = BSON(WriteConcernOptions::kWriteConcernField
                                  << WriteConcernOptions::Majority << "lsid" << lsid.toBSON()
                                  << "txnNumber" << currentTxnNumber + 1);

    hangInAdvanceTxnNumInterruptible.pauseWhileSet(opCtx);
    sendWriteCommandToRecipient(opCtx, recipientId, updateOp, passthroughFields);

    if (hangInAdvanceTxnNumThenSimulateErrorUninterruptible.shouldFail()) {
        hangInAdvanceTxnNumThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when initiating range deletion locally");
    }
}

void markAsReadyRangeDeletionTaskLocally(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto query = BSON(RangeDeletionTask::kIdFieldName << migrationId);
    auto update = BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << ""));

    hangInReadyRangeDeletionLocallyInterruptible.pauseWhileSet(opCtx);
    try {
        store.update(opCtx, query, update);
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        // If we are recovering the migration, the range-deletion may have already finished. So its
        // associated document may already have been removed.
    }

    if (hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
        hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when initiating range deletion locally");
    }
}

void resumeMigrationCoordinationsOnStepUp(OperationContext* opCtx) {
    LOGV2_DEBUG(4798510, 2, "Starting migration coordinator step-up recovery");

    unsigned long long unfinishedMigrationsCount = 0;

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.forEach(opCtx,
                  BSONObj{},
                  [&opCtx, &unfinishedMigrationsCount](const MigrationCoordinatorDocument& doc) {
                      unfinishedMigrationsCount++;
                      LOGV2_DEBUG(4798511,
                                  3,
                                  "Found unfinished migration on step-up",
                                  "migrationCoordinatorDoc"_attr = redact(doc.toBSON()),
                                  "unfinishedMigrationsCount"_attr = unfinishedMigrationsCount);

                      const auto& nss = doc.getNss();

                      {
                          AutoGetCollection autoColl(opCtx, nss, MODE_IX);
                          CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
                      }

                      asyncRecoverMigrationUntilSuccessOrStepDown(opCtx, nss);

                      return true;
                  });

    ShardingStatistics::get(opCtx).unfinishedMigrationFromPreviousPrimary.store(
        unfinishedMigrationsCount);

    LOGV2_DEBUG(4798513,
                2,
                "Finished migration coordinator step-up recovery",
                "unfinishedMigrationsCount"_attr = unfinishedMigrationsCount);
}

void recoverMigrationCoordinations(OperationContext* opCtx,
                                   NamespaceString nss,
                                   CancellationToken cancellationToken) {
    LOGV2_DEBUG(4798501, 2, "Starting migration recovery", "namespace"_attr = nss);

    unsigned migrationRecoveryCount = 0;

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.forEach(
        opCtx,
        BSON(MigrationCoordinatorDocument::kNssFieldName << nss.toString()),
        [&opCtx, &nss, &migrationRecoveryCount, &cancellationToken](
            const MigrationCoordinatorDocument& doc) {
            LOGV2_DEBUG(4798502,
                        2,
                        "Recovering migration",
                        "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));

            // Ensure there is only one migrationCoordinator document to be recovered for this
            // namespace.
            invariant(++migrationRecoveryCount == 1,
                      str::stream() << "Found more then one migration to recover for namespace '"
                                    << nss << "'");

            // Create a MigrationCoordinator to complete the coordination.
            MigrationCoordinator coordinator(doc);

            if (doc.getDecision()) {
                // The decision is already known.
                coordinator.completeMigration(opCtx);
                return true;
            }

            // The decision is not known. Recover the decision from the config server.

            ensureChunkVersionIsGreaterThan(opCtx,
                                            doc.getNss(),
                                            doc.getCollectionUuid(),
                                            doc.getRange(),
                                            doc.getPreMigrationChunkVersion());

            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(opCtx);

            auto currentMetadata = forceGetCurrentMetadata(opCtx, doc.getNss());

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for forceShardFilteringMetadataRefresh");
            }

            auto setFilteringMetadata = [&opCtx, &currentMetadata, &doc, &cancellationToken]() {
                AutoGetDb autoDb(opCtx, doc.getNss().dbName(), MODE_IX);
                Lock::CollectionLock collLock(opCtx, doc.getNss(), MODE_IX);
                auto* const csr = CollectionShardingRuntime::get(opCtx, doc.getNss());

                auto optMetadata = csr->getCurrentMetadataIfKnown();
                invariant(!optMetadata);

                auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
                if (!cancellationToken.isCanceled()) {
                    csr->setFilteringMetadata_withLock(opCtx, std::move(currentMetadata), csrLock);
                }
            };

            if (!currentMetadata.isSharded() ||
                !currentMetadata.uuidMatches(doc.getCollectionUuid())) {
                if (!currentMetadata.isSharded()) {
                    LOGV2(4798503,
                          "During migration recovery the collection was discovered to have been "
                          "dropped."
                          "Deleting the range deletion tasks on the donor and the recipient "
                          "as well as the migration coordinator document on this node",
                          "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));
                } else {
                    // UUID don't match
                    LOGV2(4798504,
                          "During migration recovery the collection was discovered to have been "
                          "dropped and recreated. Collection has a UUID that "
                          "does not match the one in the migration coordinator "
                          "document. Deleting the range deletion tasks on the donor and "
                          "recipient as well as the migration coordinator document on this node",
                          "migrationCoordinatorDocument"_attr = redact(doc.toBSON()),
                          "refreshedMetadataUUID"_attr =
                              currentMetadata.getChunkManager()->getUUID(),
                          "coordinatorDocumentUUID"_attr = doc.getCollectionUuid());
                }

                deleteRangeDeletionTaskOnRecipient(opCtx, doc.getRecipientShardId(), doc.getId());
                deleteRangeDeletionTaskLocally(opCtx, doc.getId());
                coordinator.forgetMigration(opCtx);
                setFilteringMetadata();
                return true;
            }

            // Note this should only extend the range boundaries (if there has been a shard key
            // refine since the migration began) and never truncate them.
            auto chunkRangeToCompareToMetadata =
                extendOrTruncateBoundsForMetadata(currentMetadata, doc.getRange());
            if (currentMetadata.keyBelongsToMe(chunkRangeToCompareToMetadata.getMin())) {
                coordinator.setMigrationDecision(DecisionEnum::kAborted);
            } else {
                coordinator.setMigrationDecision(DecisionEnum::kCommitted);
                if (!currentMetadata.getChunkManager()->getVersion(doc.getDonorShardId()).isSet()) {
                    notifyChangeStreamsOnDonorLastChunk(
                        opCtx, doc.getNss(), doc.getDonorShardId(), doc.getCollectionUuid());
                }
            }

            coordinator.completeMigration(opCtx);
            setFilteringMetadata();
            return true;
        });
}

ExecutorFuture<void> launchReleaseCriticalSectionOnRecipientFuture(
    OperationContext* opCtx,
    const ShardId& recipientShardId,
    const NamespaceString& nss,
    const MigrationSessionId& sessionId) {
    const auto serviceContext = opCtx->getServiceContext();
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    return ExecutorFuture<void>(executor).then([=] {
        ThreadClient tc("releaseRecipientCritSec", serviceContext);
        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc->setSystemOperationKillableByStepdown(lk);
        }
        auto uniqueOpCtx = tc->makeOperationContext();
        auto opCtx = uniqueOpCtx.get();

        const auto recipientShard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientShardId));

        BSONObjBuilder builder;
        builder.append("_recvChunkReleaseCritSec", nss.ns());
        sessionId.append(&builder);
        const auto commandObj = CommandHelpers::appendMajorityWriteConcern(builder.obj());

        retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
            opCtx,
            "release migration critical section on recipient",
            [&](OperationContext* newOpCtx) {
                try {
                    const auto response = recipientShard->runCommandWithFixedRetryAttempts(
                        newOpCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        NamespaceString::kAdminDb.toString(),
                        commandObj,
                        Shard::RetryPolicy::kIdempotent);

                    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
                } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& exShardNotFound) {
                    LOGV2(5899106,
                          "Failed to release critical section on recipient",
                          "shardId"_attr = recipientShardId,
                          "sessionId"_attr = sessionId,
                          "error"_attr = exShardNotFound);
                }
            },
            Backoff(Seconds(1), Milliseconds::max()));
    });
}

void persistMigrationRecipientRecoveryDocument(
    OperationContext* opCtx, const MigrationRecipientRecoveryDocument& migrationRecipientDoc) {
    PersistentTaskStore<MigrationRecipientRecoveryDocument> store(
        NamespaceString::kMigrationRecipientsNamespace);
    try {
        store.add(
            opCtx, migrationRecipientDoc, WriteConcerns::kMajorityWriteConcernShardingTimeout);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(6064502,
                  str::stream()
                      << "While attempting to write migration recipient information for migration "
                      << ", found document with the same migration id. Attempted migration: "
                      << migrationRecipientDoc.toBSON());
    }
}

void deleteMigrationRecipientRecoveryDocument(OperationContext* opCtx, const UUID& migrationId) {
    // Before deleting the migration recipient recovery document, ensure that in the case of a
    // crash, the node will start-up from a configTime that is inclusive of the migration that was
    // committed during the critical section.
    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

    PersistentTaskStore<MigrationRecipientRecoveryDocument> store(
        NamespaceString::kMigrationRecipientsNamespace);
    store.remove(opCtx,
                 BSON(MigrationRecipientRecoveryDocument::kIdFieldName << migrationId),
                 ShardingCatalogClient::kMajorityWriteConcern);
}

void resumeMigrationRecipientsOnStepUp(OperationContext* opCtx) {
    LOGV2_DEBUG(6064504, 2, "Starting migration recipient step-up recovery");

    unsigned long long ongoingMigrationRecipientsCount = 0;

    PersistentTaskStore<MigrationRecipientRecoveryDocument> store(
        NamespaceString::kMigrationRecipientsNamespace);

    store.forEach(
        opCtx,
        BSONObj{},
        [&opCtx, &ongoingMigrationRecipientsCount](const MigrationRecipientRecoveryDocument& doc) {
            invariant(ongoingMigrationRecipientsCount == 0,
                      str::stream()
                          << "Upon step-up a second migration recipient recovery document was found"
                          << redact(doc.toBSON()));
            ongoingMigrationRecipientsCount++;
            LOGV2_DEBUG(5899102,
                        3,
                        "Found ongoing migration recipient critical section on step-up",
                        "migrationRecipientCoordinatorDoc"_attr = redact(doc.toBSON()));

            const auto& nss = doc.getNss();

            // Register this receiveChunk on the ActiveMigrationsRegistry before completing step-up
            // to prevent a new migration from starting while a receiveChunk was ongoing. Wait for
            // any migrations that began in a previous term to complete if there are any.
            auto scopedReceiveChunk(
                uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerReceiveChunk(
                    opCtx,
                    nss,
                    doc.getRange(),
                    doc.getDonorShardIdForLoggingPurposesOnly(),
                    true /* waitForCompletionOfConflictingOps */)));

            const auto mdm = MigrationDestinationManager::get(opCtx);
            uassertStatusOK(
                mdm->restoreRecoveredMigrationState(opCtx, std::move(scopedReceiveChunk), doc));

            return true;
        });

    LOGV2_DEBUG(6064505,
                2,
                "Finished migration recipient step-up recovery",
                "ongoingRecipientCritSecCount"_attr = ongoingMigrationRecipientsCount);
}

void drainMigrationsPendingRecovery(OperationContext* opCtx) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);

    while (store.count(opCtx)) {
        store.forEach(opCtx, BSONObj(), [opCtx](const MigrationCoordinatorDocument& doc) {
            try {
                onShardVersionMismatch(opCtx, doc.getNss(), boost::none);
            } catch (DBException& ex) {
                ex.addContext(str::stream() << "Failed to recover pending migration for document "
                                            << doc.toBSON());
                throw;
            }
            return true;
        });
    }
}

void asyncRecoverMigrationUntilSuccessOrStepDown(OperationContext* opCtx,
                                                 const NamespaceString& nss) noexcept {
    ExecutorFuture<void>{Grid::get(opCtx)->getExecutorPool()->getFixedExecutor()}
        .then([svcCtx{opCtx->getServiceContext()}, nss] {
            ThreadClient tc{"MigrationRecovery", svcCtx};
            {
                stdx::lock_guard<Client> lk{*tc.get()};
                tc->setSystemOperationKillableByStepdown(lk);
            }
            auto uniqueOpCtx{tc->makeOperationContext()};
            auto opCtx{uniqueOpCtx.get()};

            try {
                refreshFilteringMetadataUntilSuccess(opCtx, nss);
            } catch (const DBException& ex) {
                // This is expected in the event of a stepdown.
                LOGV2(6316100,
                      "Interrupted deferred migration recovery",
                      "namespace"_attr = nss,
                      "error"_attr = redact(ex));
            }
        })
        .getAsync([](auto) {});
}

}  // namespace migrationutil
}  // namespace mongo
