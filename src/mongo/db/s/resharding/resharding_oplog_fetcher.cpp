/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_noop_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher_progress_gen.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <mutex>
#include <tuple>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(pauseReshardingOplogFetcherAfterConsuming);

boost::intrusive_ptr<ExpressionContext> _makeExpressionContext(OperationContext* opCtx) {
    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kRsOplogNamespace] = {NamespaceString::kRsOplogNamespace,
                                                              std::vector<BSONObj>()};
    return ExpressionContextBuilder{}
        .opCtx(opCtx)
        .mongoProcessInterface(MongoProcessInterface::create(opCtx))
        .ns(NamespaceString::kRsOplogNamespace)
        .resolvedNamespace(std::move(resolvedNamespaces))
        .allowDiskUse(true)
        .bypassDocumentValidation(true)
        .build();
}

struct OplogInsertBatch {
    std::vector<InsertStatement> statements;
    ReshardingDonorOplogId lastOplogId;
    bool moreToCome = true;
};

/**
 * Packs the oplog entries in 'aggregateBatch' into insert batches based on the configured maximum
 * insert batch count and bytes while preserving the original oplog order. If there is an oplog
 * entry whose size exceeds the max batch bytes, packs it into its own batch instead of throwing.
 * Sets the wall clock time of each oplog entry to the current wall clock time.
 */
std::vector<OplogInsertBatch> getOplogInsertBatches(OperationContext* opCtx,
                                                    const std::vector<BSONObj>& aggregateBatch) {
    size_t maxOperations = resharding::gReshardingOplogFetcherInsertBatchLimitOperations.load();
    auto maxBytes = resharding::gReshardingOplogFetcherInsertBatchLimitBytes.load();

    std::vector<OplogInsertBatch> insertBatches;
    size_t totalOperations = 0;

    size_t currentIndex = 0;
    while (currentIndex < aggregateBatch.size()) {
        OplogInsertBatch insertBatch;
        auto totalBytes = 0;

        while (currentIndex < aggregateBatch.size() &&
               insertBatch.statements.size() < maxOperations) {
            const auto& currentDoc = aggregateBatch[currentIndex];
            const auto currentBytes = currentDoc.objsize();

            if (totalBytes == 0 && currentBytes > maxBytes) {
                LOGV2_WARNING(9656101,
                              "Found a fetched oplog entry with size greater than the oplog "
                              "fetcher insert batch size limit. Ignoring the limit.",
                              "currentBytes"_attr = currentBytes,
                              "maxBytes"_attr = maxBytes);
            } else if (totalBytes + currentBytes > maxBytes) {
                break;
            }

            auto currentMutableOplogEntry =
                uassertStatusOK(repl::MutableOplogEntry::parse(currentDoc));
            currentMutableOplogEntry.setWallClockTime(opCtx->fastClockSource().now());
            auto currentMutableOplogBson = currentMutableOplogEntry.toBSON();

            insertBatch.statements.emplace_back(currentMutableOplogBson);
            totalBytes += currentBytes;
            ++currentIndex;

            insertBatch.lastOplogId = ReshardingDonorOplogId::parse(
                currentMutableOplogEntry.get_id()->getDocument().toBson(),
                IDLParserContext{"OplogFetcherParsing"});
            if (resharding::isFinalOplog({currentMutableOplogBson})) {
                insertBatch.moreToCome = false;
                break;
            }
        }

        invariant(!insertBatch.statements.empty());
        totalOperations += insertBatch.statements.size();
        const bool moreToCome = insertBatch.moreToCome;
        insertBatches.push_back(std::move(insertBatch));

        if (!moreToCome) {
            break;
        }
    }

    invariant(totalOperations == currentIndex,
              str::stream() << "Packed " << totalOperations << " after iterating upto index "
                            << currentIndex);
    return insertBatches;
}

/**
 * Inserts the oplog entries in 'oplogBatch' into the oplog buffer collection atomically (in a
 * single write unit of work).
 */
void insertOplogBatch(OperationContext* opCtx,
                      const CollectionPtr& oplogBufferColl,
                      CollectionAcquisition& oplogFetcherProgressColl,
                      const UUID& reshardingUUID,
                      const ShardId& donorShard,
                      const std::vector<InsertStatement>& oplogBatch,
                      bool storeProgress) {
    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);

    uassertStatusOK(collection_internal::insertDocuments(
        opCtx, oplogBufferColl, oplogBatch.begin(), oplogBatch.end(), nullptr));


    if (storeProgress) {
        // Update the progress doc.
        auto filter = BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName
                           << (ReshardingSourceId{reshardingUUID, donorShard}).toBSON());
        auto updateMod =
            BSON("$inc" << BSON(ReshardingOplogFetcherProgress::kNumEntriesFetchedFieldName
                                << static_cast<long long>(oplogBatch.size())));
        auto updateResult = Helpers::upsert(
            opCtx, oplogFetcherProgressColl, filter, updateMod, false /* fromMigrate */);
        invariant(updateResult.numDocsModified == 1 || !updateResult.upsertedId.isEmpty());
    }

    wuow.commit();
}

/**
 * Calculates the time it took for the last oplog entry in the latest aggregate/getMore batch to get
 * fetched.
 */
Milliseconds calculateTimeToFetch(OperationContext* opCtx,
                                  const Timer& currBatchTimer,
                                  const Timestamp& currBatchLastOplogTs,
                                  const Timestamp& prevBatchLastOplogTs) {
    if (currBatchLastOplogTs == prevBatchLastOplogTs) {
        // The cursor did not advance. That is, the cursor had reached the end of the oplog on the
        // donor, i.e. there have not been any new writes. Return the amount of time it took for
        // the aggregate or getMore command to return.
        return Milliseconds(currBatchTimer.millis());
    }
    // The cursor advanced. Return the amount of time between the current timestamp and the
    // clusterTime timestamp of the oplog entry on the donor.
    auto currentWallTime = opCtx->fastClockSource().now();
    int elapsedTimeMillis =
        currentWallTime.toMillisSinceEpoch() - currBatchLastOplogTs.getSecs() * 1000;

    // If there are clock skews, then the difference above may be negative so cap it at zero.
    return Milliseconds(std::max(0, elapsedTimeMillis));
}

}  // namespace

const ReshardingDonorOplogId ReshardingOplogFetcher::kFinalOpAlreadyFetched{Timestamp::max(),
                                                                            Timestamp::max()};

ReshardingOplogFetcher::ReshardingOplogFetcher(std::unique_ptr<Env> env,
                                               UUID reshardingUUID,
                                               UUID collUUID,
                                               ReshardingDonorOplogId startAt,
                                               ShardId donorShard,
                                               ShardId recipientShard,
                                               NamespaceString oplogBufferNss,
                                               bool storeProgress)
    : _env(std::move(env)),
      _reshardingUUID(reshardingUUID),
      _collUUID(collUUID),
      _donorShard(donorShard),
      _recipientShard(recipientShard),
      _oplogBufferNss(oplogBufferNss),
      _storeProgress(storeProgress),
      _startAt(startAt) {
    auto [p, f] = makePromiseFuture<void>();
    stdx::lock_guard lk(_mutex);
    _onInsertPromise = std::move(p);
    _onInsertFuture = std::move(f);
}

ReshardingOplogFetcher::~ReshardingOplogFetcher() {
    stdx::lock_guard lk(_mutex);
    _onInsertPromise.setError(
        {ErrorCodes::CallbackCanceled, "explicitly breaking promise from ReshardingOplogFetcher"});
}

Future<void> ReshardingOplogFetcher::awaitInsert(const ReshardingDonorOplogId& lastSeen) {
    // `lastSeen` is the _id of the document ReshardingDonorOplogIterator::getNextBatch() has last
    // read from the oplog buffer collection.
    //
    // `_startAt` is updated after each insert into the oplog buffer collection by
    // ReshardingOplogFetcher to reflect the newer resume point if a new aggregation request was
    // being issued. It is also updated with the latest oplog timestamp from donor's cursor response
    // after we finish inserting the entire batch.
    stdx::lock_guard lk(_mutex);
    if (lastSeen < _startAt) {
        // `lastSeen < _startAt` means there's at least one document which has been inserted by
        // ReshardingOplogFetcher and hasn't been returned by
        // ReshardingDonorOplogIterator::getNextBatch(). The caller has no reason to wait until yet
        // another document has been inserted before reading from the oplog buffer collection.
        return Future<void>::makeReady();
    }

    // `lastSeen == _startAt` means the last document inserted by ReshardingOplogFetcher has already
    // been returned by ReshardingDonorOplogIterator::getNextBatch() and so
    // ReshardingDonorOplogIterator would want to wait until ReshardingOplogFetcher does another
    // insert.
    //
    // `lastSeen > _startAt` isn't expected to happen in practice because
    // ReshardingDonorOplogIterator only uses _id's from documents that it actually read from the
    // oplog buffer collection for `lastSeen`, but would also mean the caller wants to wait.
    return std::move(_onInsertFuture);
}

ExecutorFuture<void> ReshardingOplogFetcher::schedule(
    std::shared_ptr<executor::TaskExecutor> executor, const CancellationToken& cancelToken) {
    if (_startAt == kFinalOpAlreadyFetched) {
        LOGV2_INFO(6077400,
                   "Resharding oplog fetcher resumed with no more work to do",
                   "donorShard"_attr = _donorShard,
                   "reshardingUUID"_attr = _reshardingUUID);
        return ExecutorFuture(std::move(executor));
    }

    return ExecutorFuture(executor)
        .then([this, executor, cancelToken]() mutable {
            return _reschedule(std::move(executor), cancelToken);
        })
        .onError([](Status status) {
            LOGV2_INFO(5192101, "Resharding oplog fetcher aborting", "reason"_attr = status);
            return status;
        });
}

ExecutorFuture<void> ReshardingOplogFetcher::_reschedule(
    std::shared_ptr<executor::TaskExecutor> executor, const CancellationToken& cancelToken) {
    auto delay = std::make_shared<Milliseconds>(0);
    return AsyncTry([this, executor, cancelToken, delay] {
               return executor->sleepFor(*delay, cancelToken).then([this, executor, cancelToken] {
                   if (_startAt == kFinalOpAlreadyFetched) {
                       LOGV2_INFO(10355401,
                                  "Not rescheduling resharding oplog fetcher since there is no "
                                  "more work to do",
                                  "donorShard"_attr = _donorShard,
                                  "reshardingUUID"_attr = _reshardingUUID);
                       return false;
                   }

                   // TODO(SERVER-74658): Please revisit if this thread could be made killable.
                   ThreadClient client(fmt::format("OplogFetcher-{}-{}",
                                                   _reshardingUUID.toString(),
                                                   _donorShard.toString()),
                                       _service()->getService(ClusterRole::ShardServer),
                                       ClientOperationKillableByStepdown{false});

                   boost::optional<CancelableOperationContextFactory> aggOpCtxFactory;
                   {
                       stdx::lock_guard lk(_mutex);
                       _aggCancelSource.emplace(cancelToken);
                       aggOpCtxFactory.emplace(_aggCancelSource->token(), executor);
                   }
                   return iterate(client.get(), *aggOpCtxFactory);
               });
           })
        .until([this, executor, cancelToken, delay](StatusWith<bool> statusWithMoreOplogsToCome) {
            if (!statusWithMoreOplogsToCome.isOK() || !statusWithMoreOplogsToCome.getValue() ||
                cancelToken.isCanceled()) {
                return true;
            } else {
                auto sleepDuration = Milliseconds{
                    _isPreparingForCriticalSection.load()
                        ? resharding::gReshardingOplogFetcherSleepMillisDuringCriticalSection.load()
                        : resharding::gReshardingOplogFetcherSleepMillisBeforeCriticalSection
                              .load()};
                *delay = sleepDuration;
                return false;
            }
        })
        .on(executor, cancelToken)
        .onCompletion([this, executor, cancelToken](StatusWith<bool> statusWith) {
            if (cancelToken.isCanceled()) {
                return ExecutorFuture<void>(
                    std::move(executor),
                    Status{ErrorCodes::CallbackCanceled,
                           "Resharding oplog fetcher canceled due to abort or stepdown"});
            }
            if (!statusWith.isOK()) {
                return ExecutorFuture<void>(std::move(executor), statusWith.getStatus());
            } else {
                auto moreOplogsToCome = statusWith.getValue();
                // If the cancelToken is not canceled then moreToCome has to be false at this point
                // for us to have exited the until condition of AsyncTry.
                invariant(!moreOplogsToCome);
                LOGV2_INFO(6077401,
                           "Resharding oplog fetcher done fetching",
                           "donorShard"_attr = _donorShard,
                           "reshardingUUID"_attr = _reshardingUUID);
                return ExecutorFuture(std::move(executor));
            }
        });
}

bool ReshardingOplogFetcher::iterate(Client* client, CancelableOperationContextFactory factory) {
    try {
        std::shared_ptr<Shard> targetShard;
        {
            auto opCtxRaii = factory.makeOperationContext(client);
            opCtxRaii->checkForInterrupt();

            StatusWith<std::shared_ptr<Shard>> swDonor =
                Grid::get(opCtxRaii.get())->shardRegistry()->getShard(opCtxRaii.get(), _donorShard);
            if (!swDonor.isOK()) {
                LOGV2_WARNING(5127203,
                              "Error finding shard in registry, retrying.",
                              "error"_attr = swDonor.getStatus());
                return true;
            }
            targetShard = swDonor.getValue();
        }
        return consume(client, factory, targetShard.get());
    } catch (const ExceptionFor<ErrorCodes::OplogQueryMinTsMissing>&) {
        LOGV2_ERROR(
            5192103, "Fatal resharding error while fetching.", "error"_attr = exceptionToStatus());
        throw;
    } catch (const ExceptionFor<ErrorCodes::IncompleteTransactionHistory>&) {
        LOGV2_ERROR(
            5354400, "Fatal resharding error while fetching.", "error"_attr = exceptionToStatus());
        throw;
    } catch (const DBException&) {
        LOGV2_WARNING(
            5127200, "Error while fetching, retrying.", "error"_attr = exceptionToStatus());
        return true;
    }
}

void ReshardingOplogFetcher::_ensureCollection(Client* client,
                                               CancelableOperationContextFactory factory,
                                               const NamespaceString nss) {
    auto opCtxRaii = factory.makeOperationContext(client);
    auto opCtx = opCtxRaii.get();
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Create the destination collection if necessary.
    writeConflictRetry(opCtx, "createReshardingLocalOplogBuffer", nss, [&] {
        const Collection* coll =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        if (coll) {
            return;
        }

        WriteUnitOfWork wuow(opCtx);
        AutoGetDb autoDb(opCtx, nss.dbName(), LockMode::MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto db = autoDb.ensureDbExists(opCtx);

        // This oplog-like collection will benefit from clustering by _id to reduce storage overhead
        // and improve _id query efficiency.
        CollectionOptions options;
        options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
        db->createCollection(opCtx, nss, options);
        wuow.commit();
    });
}

AggregateCommandRequest ReshardingOplogFetcher::_makeAggregateCommandRequest(
    Client* client, CancelableOperationContextFactory factory) {
    auto opCtxRaii = factory.makeOperationContext(client);
    auto opCtx = opCtxRaii.get();
    auto expCtx = _makeExpressionContext(opCtx);

    auto serializedPipeline = resharding::createOplogFetchingPipelineForResharding(
                                  expCtx, _startAt, _collUUID, _recipientShard)
                                  ->serializeToBson();

    AggregateCommandRequest aggRequest(NamespaceString::kRsOplogNamespace,
                                       std::move(serializedPipeline));
    if (_useReadConcern) {
        // We specify {afterClusterTime: <highest _id.clusterTime>, level: "majority"} as the read
        // concern to guarantee the postBatchResumeToken when the batch is empty is non-decreasing.
        // The ReshardingOplogFetcher depends on inserting documents in increasing _id order,
        // including for the synthetic no-op oplog entries generated from the postBatchResumeToken.
        invariant(_startAt != kFinalOpAlreadyFetched);
        auto readConcernArgs = repl::ReadConcernArgs(
            boost::optional<LogicalTime>(_startAt.getClusterTime()),
            boost::optional<repl::ReadConcernLevel>(repl::ReadConcernLevel::kMajorityReadConcern));
        aggRequest.setReadConcern(readConcernArgs);
    }

    auto readPref = _isPreparingForCriticalSection.load() &&
            resharding::gReshardingOplogFetcherTargetPrimaryDuringCriticalSection.load()
        ? ReadPreferenceSetting{ReadPreference::PrimaryOnly}
        : ReadPreferenceSetting{ReadPreference::Nearest,
                                ReadPreferenceSetting::kMinimalMaxStalenessValue};
    aggRequest.setUnwrappedReadPref(BSON("$readPreference" << readPref.toInnerBSON()));

    aggRequest.setWriteConcern(WriteConcernOptions());
    aggRequest.setHint(BSON("$natural" << 1));
    aggRequest.setRequestReshardingResumeToken(true);

    if (_initialBatchSize) {
        SimpleCursorOptions cursor;
        cursor.setBatchSize(_initialBatchSize);
        aggRequest.setCursor(cursor);
    }

    return aggRequest;
}

bool ReshardingOplogFetcher::_needToEstimateRemainingTimeBasedOnMovingAverage(
    OperationContext* opCtx) {
    if (!_supportEstimatingRemainingTimeBasedOnMovingAverage.has_value()) {
        // Only check the feature flag once since the setFCV command aborts any in-progress
        // resharding operation so no resharding operations can span multiple FCV versions.
        _supportEstimatingRemainingTimeBasedOnMovingAverage =
            resharding::gFeatureFlagReshardingRemainingTimeEstimateBasedOnMovingAverage.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    }

    return *_supportEstimatingRemainingTimeBasedOnMovingAverage &&
        resharding::gReshardingRemainingTimeEstimateBasedOnMovingAverage.load();
}

bool ReshardingOplogFetcher::_needToUpdateAverageTimeToApply(WithLock,
                                                             OperationContext* opCtx) const {
    if (!_oplogApplicationStarted.load()) {
        return false;
    }

    if (!_lastUpdatedProgressMarkAt) {
        return true;
    }

    auto elapsedTime = opCtx->fastClockSource().now() - *_lastUpdatedProgressMarkAt;
    return durationCount<Milliseconds>(elapsedTime) >=
        resharding::gReshardingExponentialMovingAverageTimeToFetchAndApplyIntervalMillis.load();
}

boost::optional<ReshardingDonorOplogId>
ReshardingOplogFetcher::_makeProgressMarkOplogIdIfNeedToInsert(
    OperationContext* opCtx, const Timestamp& currBatchLastOplogTs) {
    stdx::lock_guard lk(_mutex);

    // If the recipient has been configured to estimate the remaining time based on moving average
    // of the time it takes to fetch and apply oplog entries, write a progress mark noop oplog
    // entry if the average time to apply needs to updated since it is the oplog entry used for
    // recalculating the average.
    if (_needToEstimateRemainingTimeBasedOnMovingAverage(opCtx) &&
        _needToUpdateAverageTimeToApply(lk, opCtx)) {
        auto oplogId = ReshardingDonorOplogId(currBatchLastOplogTs, currBatchLastOplogTs);
        // Only set a progress mark id if the oplog entry is needed for updating the average time to
        // apply oplog entries since:
        // - The progress mark id was introduced in 8.2 to avoid DuplicateKey error when inserting
        //   an existing progress timestamp, which is only permitted when the oplog entry is needed
        //   for calculating the average time to apply.
        // - Prior to 8.2, ReshardingDonorOplogId used strict parsing so the presence of an unknown
        //   field is expected to lead parsing errors.
        if (currBatchLastOplogTs == _startAt.getTs()) {
            oplogId.setProgressMarkId(
                _startAt.getProgressMarkId() ? *_startAt.getProgressMarkId() + 1 : 0);
        } else {
            oplogId.setProgressMarkId(0);
        }
        return std::move(oplogId);
    }

    // Otherwise, only write it if currBatchLastOplogTs is not equal to the timestamp of '_startAt'
    // since:
    //   1. There is already an oplog with timestamp equal to 'currBatchLastOplogTs' in the
    //      oplogBufferColl collection from which the fetcher knows to resume from.
    //   2. Or currBatchLastOplogTs equals the initial value for _startAt
    //     (typically the minFetchTimestamp) in which case, the fetcher is going to
    //     resume from there anyways.
    if (currBatchLastOplogTs != _startAt.getTs()) {
        auto oplogId = ReshardingDonorOplogId(currBatchLastOplogTs, currBatchLastOplogTs);
        return std::move(oplogId);
    }

    return boost::none;
}

repl::MutableOplogEntry ReshardingOplogFetcher::_makeProgressMarkOplog(
    OperationContext* opCtx, const ReshardingDonorOplogId& oplogId) const {
    repl::MutableOplogEntry oplog;

    oplog.setNss(_oplogBufferNss);
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(_collUUID);
    oplog.set_id(Value(oplogId.toBSON()));
    oplog.setObject(BSON("msg" << "Latest oplog ts from donor's cursor response"));

    ReshardProgressMarkO2Field o2Field;
    o2Field.setType(resharding::kReshardProgressMarkOpLogType);
    if (_oplogApplicationStarted.load()) {
        o2Field.setCreatedAfterOplogApplicationStarted(true);
    }
    oplog.setObject2(o2Field.toBSON());

    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(opCtx->fastClockSource().now());
    return oplog;
}

bool ReshardingOplogFetcher::consume(Client* client,
                                     CancelableOperationContextFactory factory,
                                     Shard* shard) {
    _ensureCollection(client, factory, _oplogBufferNss);

    auto aggRequest = _makeAggregateCommandRequest(client, factory);

    auto opCtxRaii = factory.makeOperationContext(client);
    int batchesProcessed = 0;
    bool moreToCome = true;

    auto tickSource = opCtxRaii->getServiceContext()->getTickSource();
    Timer batchTimer(tickSource);

    // Note that the oplog entries are *not* being copied with a tailable cursor.
    // Shard::runAggregation() will instead return upon hitting the end of the donor's oplog.
    uassertStatusOK(shard->runAggregation(
        opCtxRaii.get(),
        aggRequest,
        [this, &batchesProcessed, &moreToCome, &opCtxRaii, &batchTimer, factory](
            const std::vector<BSONObj>& aggregateBatch,
            const boost::optional<BSONObj>& postBatchResumeToken) {
            _env->metrics()->onBatchRetrievedDuringOplogFetching(Milliseconds(batchTimer.millis()));

            // TODO(SERVER-74658): Please revisit if this thread could be made killable.
            ThreadClient client(fmt::format("ReshardingFetcher-{}-{}",
                                            _reshardingUUID.toString(),
                                            _donorShard.toString()),
                                _service()->getService(ClusterRole::ShardServer),
                                ClientOperationKillableByStepdown{false});
            auto opCtxRaii = factory.makeOperationContext(client.get());
            auto opCtx = opCtxRaii.get();

            auto prevBatchLastOplogId = [&] {
                stdx::lock_guard lk(_mutex);
                return _startAt;
            }();

            // Noting some possible optimizations:
            //
            // * Parallize writing documents across multiple threads.
            // * Doing the above while still using the underlying message buffer of bson objects.
            const auto oplogBufferColl =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      _oplogBufferNss,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kWrite),
                                  MODE_IX);
            auto oplogFetcherProgressColl =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      NamespaceString::kReshardingFetcherProgressNamespace,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kWrite),
                                  MODE_IX);

            const auto insertBatches = getOplogInsertBatches(opCtx, aggregateBatch);
            for (const auto& insertBatch : insertBatches) {
                Timer insertTimer;

                insertOplogBatch(opCtx,
                                 oplogBufferColl.getCollectionPtr(),
                                 oplogFetcherProgressColl,
                                 _reshardingUUID,
                                 _donorShard,
                                 insertBatch.statements,
                                 _storeProgress);

                _env->metrics()->onLocalInsertDuringOplogFetching(
                    Milliseconds(insertTimer.millis()));
                _env->metrics()->onOplogEntriesFetched(insertBatch.statements.size());
                _numOplogEntriesCopied += insertBatch.statements.size();

                auto [p, f] = makePromiseFuture<void>();
                {
                    stdx::lock_guard lk(_mutex);
                    _startAt = insertBatch.moreToCome
                        ? insertBatch.lastOplogId
                        : ReshardingOplogFetcher::kFinalOpAlreadyFetched;
                    _onInsertPromise.emplaceValue();
                    _onInsertPromise = std::move(p);
                    _onInsertFuture = std::move(f);
                }

                pauseReshardingOplogFetcherAfterConsuming.pauseWhileSet(opCtx);

                if (!insertBatch.moreToCome) {
                    moreToCome = false;
                    return false;
                }
            }

            if (postBatchResumeToken) {
                auto currBatchLastOplogTs = postBatchResumeToken->getField("ts").timestamp();

                // Insert a noop entry with the latest oplog timestamp from the donor's cursor
                // response. This will allow the fetcher to resume reading from the last oplog entry
                // it fetched even if that entry is for a different collection, making resuming less
                // wasteful.
                if (auto oplogId =
                        _makeProgressMarkOplogIdIfNeedToInsert(opCtx, currBatchLastOplogTs)) {
                    auto oplog = _makeProgressMarkOplog(opCtx, *oplogId);

                    try {
                        insertOplogBatch(opCtx,
                                         oplogBufferColl.getCollectionPtr(),
                                         oplogFetcherProgressColl,
                                         _reshardingUUID,
                                         _donorShard,
                                         {InsertStatement{oplog.toBSON()}},
                                         _storeProgress);
                        // Also include synthetic oplog in the fetched count
                        // so it can match up with the total oplog applied
                        // count in the end.
                        _env->metrics()->onOplogEntriesFetched(1);

                        auto [p, f] = makePromiseFuture<void>();
                        {
                            stdx::lock_guard lk(_mutex);
                            _startAt = *oplogId;
                            _lastUpdatedProgressMarkAt = opCtx->fastClockSource().now();
                            _onInsertPromise.emplaceValue();
                            _onInsertPromise = std::move(p);
                            _onInsertFuture = std::move(f);
                        }
                    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
                        // It's possible that the donor shard has not generated new oplog entries
                        // since the previous getMore. In this case the latest oplog timestamp the
                        // donor returns will be the same, so it's safe to ignore this error.
                    }
                }

                auto timeToFetch = calculateTimeToFetch(
                    opCtx, batchTimer, currBatchLastOplogTs, prevBatchLastOplogId.getTs());
                _env->metrics()->updateAverageTimeToFetchOplogEntries(_donorShard, timeToFetch);
            }

            batchTimer.reset();

            if (_maxBatches > -1 && ++batchesProcessed >= _maxBatches) {
                return false;
            }

            return true;
        }));

    return moreToCome;
}

void ReshardingOplogFetcher::onStartingOplogApplication() {
    _oplogApplicationStarted.store(true);
}

void ReshardingOplogFetcher::prepareForCriticalSection() {
    stdx::lock_guard lk(_mutex);

    if (_isPreparingForCriticalSection.load()) {
        return;
    }

    _isPreparingForCriticalSection.store(true);
    // Stop consuming the current aggregation and start a new one.
    if (resharding::gReshardingOplogFetcherTargetPrimaryDuringCriticalSection.load() &&
        _aggCancelSource) {
        _aggCancelSource->cancel();
    }
}

}  // namespace mongo
