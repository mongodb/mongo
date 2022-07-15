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


#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/mutex.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {
namespace {
boost::intrusive_ptr<ExpressionContext> _makeExpressionContext(OperationContext* opCtx) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kRsOplogNamespace.coll()] = {
        NamespaceString::kRsOplogNamespace, std::vector<BSONObj>()};
    return make_intrusive<ExpressionContext>(opCtx,
                                             boost::none, /* explain */
                                             false,       /* fromMongos */
                                             false,       /* needsMerge */
                                             true,        /* allowDiskUse */
                                             true,        /* bypassDocumentValidation */
                                             false,       /* isMapReduceCommand */
                                             NamespaceString::kRsOplogNamespace,
                                             boost::none, /* runtimeConstants */
                                             nullptr,     /* collator */
                                             MongoProcessInterface::create(opCtx),
                                             std::move(resolvedNamespaces),
                                             boost::none); /* collUUID */
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
                                               NamespaceString toWriteInto)
    : _env(std::move(env)),
      _reshardingUUID(reshardingUUID),
      _collUUID(collUUID),
      _startAt(startAt),
      _donorShard(donorShard),
      _recipientShard(recipientShard),
      _toWriteInto(toWriteInto) {
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
    std::shared_ptr<executor::TaskExecutor> executor,
    const CancellationToken& cancelToken,
    CancelableOperationContextFactory factory) {
    if (_startAt == kFinalOpAlreadyFetched) {
        LOGV2_INFO(6077400,
                   "Resharding oplog fetcher resumed with no more work to do",
                   "donorShard"_attr = _donorShard,
                   "reshardingUUID"_attr = _reshardingUUID);
        return ExecutorFuture(std::move(executor));
    }

    return ExecutorFuture(executor)
        .then([this, executor, cancelToken, factory] {
            return _reschedule(std::move(executor), cancelToken, factory);
        })
        .onError([](Status status) {
            LOGV2_INFO(5192101, "Resharding oplog fetcher aborting", "reason"_attr = status);
            return status;
        });
}

ExecutorFuture<void> ReshardingOplogFetcher::_reschedule(
    std::shared_ptr<executor::TaskExecutor> executor,
    const CancellationToken& cancelToken,
    CancelableOperationContextFactory factory) {
    return ExecutorFuture(executor)
        .then([this, executor, cancelToken, factory] {
            ThreadClient client(fmt::format("OplogFetcher-{}-{}",
                                            _reshardingUUID.toString(),
                                            _donorShard.toString()),
                                _service());
            return iterate(client.get(), factory);
        })
        .then([executor, cancelToken](bool moreToCome) {
            // Wait a little before re-running the aggregation pipeline on the donor's oplog. The
            // 1-second value was chosen to match the default awaitData timeout that would have been
            // used if the aggregation cursor was TailableModeEnum::kTailableAndAwaitData.
            return executor->sleepFor(Seconds{1}, cancelToken).then([moreToCome] {
                return moreToCome;
            });
        })
        .then([this, executor, cancelToken, factory](bool moreToCome) {
            if (!moreToCome) {
                LOGV2_INFO(6077401,
                           "Resharding oplog fetcher done fetching",
                           "donorShard"_attr = _donorShard,
                           "reshardingUUID"_attr = _reshardingUUID);
                return ExecutorFuture(std::move(executor));
            }

            if (cancelToken.isCanceled()) {
                return ExecutorFuture<void>(
                    executor,
                    Status{ErrorCodes::CallbackCanceled,
                           "Resharding oplog fetcher canceled due to abort or stepdown"});
            }
            return _reschedule(std::move(executor), cancelToken, factory);
        });
}

bool ReshardingOplogFetcher::iterate(Client* client, CancelableOperationContextFactory factory) {
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

    try {
        return consume(client, factory, targetShard.get());
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        // Defer to the cancellation token for whether the Interruption exception should be retried
        // upon or not. It is possible the error came as the response from the remote donor shard.
        // It is also possible that the error is from a stray killOp command being run on this
        // recipient shard.
        return true;
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
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Create the destination collection if necessary.
    writeConflictRetry(opCtx, "createReshardingLocalOplogBuffer", nss.toString(), [&] {
        const CollectionPtr coll =
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
        aggRequest.setReadConcern(readConcernArgs.toBSONInner());
    }

    ReadPreferenceSetting readPref(ReadPreference::Nearest,
                                   ReadPreferenceSetting::kMinimalMaxStalenessValue);
    aggRequest.setUnwrappedReadPref(readPref.toContainingBSON());

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

bool ReshardingOplogFetcher::consume(Client* client,
                                     CancelableOperationContextFactory factory,
                                     Shard* shard) {
    _ensureCollection(client, factory, _toWriteInto);

    auto aggRequest = _makeAggregateCommandRequest(client, factory);

    auto opCtxRaii = factory.makeOperationContext(client);
    int batchesProcessed = 0;
    bool moreToCome = true;
    Timer batchFetchTimer;
    // Note that the oplog entries are *not* being copied with a tailable cursor.
    // Shard::runAggregation() will instead return upon hitting the end of the donor's oplog.
    uassertStatusOK(shard->runAggregation(
        opCtxRaii.get(),
        aggRequest,
        [this, &batchesProcessed, &moreToCome, &opCtxRaii, &batchFetchTimer, factory](
            const std::vector<BSONObj>& batch,
            const boost::optional<BSONObj>& postBatchResumeToken) {
            _env->metrics()->onOplogEntriesFetched(batch.size(),
                                                   Milliseconds(batchFetchTimer.millis()));

            ThreadClient client(fmt::format("ReshardingFetcher-{}-{}",
                                            _reshardingUUID.toString(),
                                            _donorShard.toString()),
                                _service(),
                                nullptr);
            auto opCtxRaii = factory.makeOperationContext(client.get());
            auto opCtx = opCtxRaii.get();

            // Noting some possible optimizations:
            //
            // * Batch more inserts into larger storage transactions.
            // * Parallize writing documents across multiple threads.
            // * Doing either of the above while still using the underlying message buffer of bson
            //   objects.
            AutoGetCollection toWriteTo(opCtx, _toWriteInto, LockMode::MODE_IX);
            for (const BSONObj& doc : batch) {
                WriteUnitOfWork wuow(opCtx);
                auto nextOplog = uassertStatusOK(repl::OplogEntry::parse(doc));

                auto startAt = ReshardingDonorOplogId::parse(
                    {"OplogFetcherParsing"}, nextOplog.get_id()->getDocument().toBson());
                Timer insertTimer;
                uassertStatusOK(toWriteTo->insertDocument(opCtx, InsertStatement{doc}, nullptr));
                wuow.commit();

                _env->metrics()->onLocalInsertDuringOplogFetching(
                    Milliseconds(insertTimer.millis()));

                ++_numOplogEntriesCopied;

                auto [p, f] = makePromiseFuture<void>();
                {
                    stdx::lock_guard lk(_mutex);
                    _startAt = startAt;
                    _onInsertPromise.emplaceValue();
                    _onInsertPromise = std::move(p);
                    _onInsertFuture = std::move(f);
                }

                if (resharding::isFinalOplog(nextOplog, _reshardingUUID)) {
                    moreToCome = false;
                    return false;
                }
            }

            if (postBatchResumeToken) {
                // Insert a noop entry with the latest oplog timestamp from the donor's cursor
                // response. This will allow the fetcher to resume reading from the last oplog entry
                // it fetched even if that entry is for a different collection, making resuming less
                // wasteful.
                try {
                    auto lastOplogTs = postBatchResumeToken->getField("ts").timestamp();
                    auto startAt = ReshardingDonorOplogId(lastOplogTs, lastOplogTs);

                    WriteUnitOfWork wuow(opCtx);

                    repl::MutableOplogEntry oplog;
                    oplog.setNss(_toWriteInto);
                    oplog.setOpType(repl::OpTypeEnum::kNoop);
                    oplog.setUuid(_collUUID);
                    oplog.set_id(Value(startAt.toBSON()));
                    oplog.setObject(BSON("msg"
                                         << "Latest oplog ts from donor's cursor response"));
                    oplog.setObject2(BSON("type" << resharding::kReshardProgressMark));
                    oplog.setOpTime(OplogSlot());
                    oplog.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());

                    uassertStatusOK(
                        toWriteTo->insertDocument(opCtx, InsertStatement{oplog.toBSON()}, nullptr));
                    wuow.commit();

                    // Also include synthetic oplog in the fetched count so it can match up with the
                    // total oplog applied count in the end.
                    _env->metrics()->onOplogEntriesFetched(1, Milliseconds(0));

                    auto [p, f] = makePromiseFuture<void>();
                    {
                        stdx::lock_guard lk(_mutex);
                        _startAt = startAt;
                        _onInsertPromise.emplaceValue();
                        _onInsertPromise = std::move(p);
                        _onInsertFuture = std::move(f);
                    }
                } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
                    // It's possible that the donor shard has not generated new oplog entries since
                    // the previous getMore. In this case the latest oplog timestamp the donor
                    // returns will be the same, so it's safe to ignore this error.
                }
            }

            if (_maxBatches > -1 && ++batchesProcessed >= _maxBatches) {
                return false;
            }

            return true;
        }));

    return moreToCome;
}

}  // namespace mongo
