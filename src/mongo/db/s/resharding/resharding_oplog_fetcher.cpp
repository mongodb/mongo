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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {
boost::intrusive_ptr<ExpressionContext> _makeExpressionContext(OperationContext* opCtx) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    const NamespaceString slimOplogNs("local.system.resharding.slimOplogForGraphLookup");
    resolvedNamespaces[slimOplogNs.coll()] = {slimOplogNs, std::vector<BSONObj>()};
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

ReshardingOplogFetcher::ReshardingOplogFetcher(UUID reshardingUUID,
                                               UUID collUUID,
                                               ReshardingDonorOplogId startAt,
                                               ShardId donorShard,
                                               ShardId recipientShard,
                                               bool doesDonorOwnMinKeyChunk,
                                               NamespaceString toWriteInto)
    : _reshardingUUID(reshardingUUID),
      _collUUID(collUUID),
      _startAt(startAt),
      _donorShard(donorShard),
      _recipientShard(recipientShard),
      _doesDonorOwnMinKeyChunk(doesDonorOwnMinKeyChunk),
      _toWriteInto(toWriteInto) {}

Future<void> ReshardingOplogFetcher::schedule(executor::TaskExecutor* executor) {
    auto pf = makePromiseFuture<void>();
    _fetchedFinishPromise = std::move(pf.promise);

    _reschedule(executor);

    return std::move(pf.future);
}

void ReshardingOplogFetcher::_reschedule(executor::TaskExecutor* executor) {
    executor->schedule([this, executor](Status status) {
        ThreadClient client(
            fmt::format("OplogFetcher-{}-{}", _reshardingUUID.toString(), _donorShard.toString()),
            getGlobalServiceContext());
        if (!status.isOK()) {
            LOGV2_INFO(5192101, "Resharding oplog fetcher aborting.", "reason"_attr = status);
            _fetchedFinishPromise.setError(status);
            return;
        }

        try {
            if (iterate(client.get())) {
                _reschedule(executor);
            } else {
                _fetchedFinishPromise.emplaceValue();
            }
        } catch (...) {
            LOGV2_INFO(5192102, "Error.", "reason"_attr = exceptionToStatus());
            _fetchedFinishPromise.setError(exceptionToStatus());
        }
    });
}

bool ReshardingOplogFetcher::iterate(Client* client) {
    std::shared_ptr<Shard> targetShard;
    {
        auto opCtxRaii = client->makeOperationContext();
        opCtxRaii->checkForInterrupt();

        const Seconds maxStaleness(10);
        ReadPreferenceSetting readPref(ReadPreference::Nearest, maxStaleness);
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
        // Consume will throw if there's oplog entries to be copied. It only returns cleanly when
        // the final oplog has been seen and copied.
        consume(client, targetShard.get());
        return false;
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        return false;
    } catch (const ExceptionFor<ErrorCodes::OplogQueryMinTsMissing>&) {
        LOGV2_ERROR(
            5192103, "Fatal resharding error while fetching.", "error"_attr = exceptionToStatus());
        throw;
    } catch (const DBException&) {
        LOGV2_WARNING(
            5127200, "Error while fetching, retrying.", "error"_attr = exceptionToStatus());
        return true;
    }
}

void ReshardingOplogFetcher::_ensureCollection(Client* client, const NamespaceString nss) {
    auto opCtxRaii = client->makeOperationContext();
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
        AutoGetOrCreateDb db(opCtx, nss.db(), LockMode::MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        db.getDb()->createCollection(opCtx, nss);
        wuow.commit();
    });
}

std::vector<BSONObj> ReshardingOplogFetcher::_makePipeline(Client* client) {
    auto opCtxRaii = client->makeOperationContext();
    auto opCtx = opCtxRaii.get();
    auto expCtx = _makeExpressionContext(opCtx);

    return createOplogFetchingPipelineForResharding(
               expCtx, _startAt, _collUUID, _recipientShard, _doesDonorOwnMinKeyChunk)
        ->serializeToBson();
}

void ReshardingOplogFetcher::consume(Client* client, Shard* shard) {
    _ensureCollection(client, _toWriteInto);
    std::vector<BSONObj> serializedPipeline = _makePipeline(client);

    AggregationRequest aggRequest(NamespaceString::kRsOplogNamespace, serializedPipeline);
    if (_useReadConcern) {
        auto readConcernArgs = repl::ReadConcernArgs(
            boost::optional<LogicalTime>(_startAt.getTs()),
            boost::optional<repl::ReadConcernLevel>(repl::ReadConcernLevel::kMajorityReadConcern));
        aggRequest.setReadConcern(readConcernArgs.toBSONInner());
    }

    aggRequest.setHint(BSON("$natural" << 1));
    aggRequest.setRequestReshardingResumeToken(true);

    if (_initialBatchSize) {
        aggRequest.setBatchSize(_initialBatchSize);
    }

    auto opCtxRaii = client->makeOperationContext();
    int batchesProcessed = 0;
    auto svcCtx = client->getServiceContext();
    uassertStatusOK(shard->runAggregation(
        opCtxRaii.get(),
        aggRequest,
        [this, svcCtx, &batchesProcessed](const std::vector<BSONObj>& batch) {
            ThreadClient client(fmt::format("ReshardingFetcher-{}-{}",
                                            _reshardingUUID.toString(),
                                            _donorShard.toString()),
                                svcCtx,
                                nullptr);
            auto opCtxRaii = cc().makeOperationContext();
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

                _startAt = ReshardingDonorOplogId::parse(
                    {"OplogFetcherParsing"}, nextOplog.get_id()->getDocument().toBson());
                uassertStatusOK(toWriteTo->insertDocument(opCtx, InsertStatement{doc}, nullptr));
                wuow.commit();
                ++_numOplogEntriesCopied;

                if (isFinalOplog(nextOplog, _reshardingUUID)) {
                    return false;
                }
            }

            if (_maxBatches > -1 && ++batchesProcessed >= _maxBatches) {
                return false;
            }

            return true;
        }));
}

}  // namespace mongo
