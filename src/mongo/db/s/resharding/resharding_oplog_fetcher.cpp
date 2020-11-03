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
      _toWriteInto(toWriteInto),
      _client(getGlobalServiceContext()->makeClient(
          fmt::format("OplogFetcher-{}-{}", reshardingUUID.toString(), donorShard.toString()))) {}

void ReshardingOplogFetcher::consume(DBClientBase* conn) {
    while (true) {
        auto opCtxRaii = _client->makeOperationContext();
        opCtxRaii->checkForInterrupt();
        auto expCtx = _makeExpressionContext(opCtxRaii.get());
        boost::optional<ReshardingDonorOplogId> restartAt = iterate(opCtxRaii.get(),
                                                                    conn,
                                                                    expCtx,
                                                                    _startAt,
                                                                    _collUUID,
                                                                    _recipientShard,
                                                                    _doesDonorOwnMinKeyChunk,
                                                                    _toWriteInto);
        if (!restartAt) {
            return;
        }
        _startAt = restartAt.get();
    }
}

void ReshardingOplogFetcher::setKilled() {
    _isAlive.store(false);
    _client->setKilled();
}

void ReshardingOplogFetcher::schedule(executor::TaskExecutor* executor) {
    executor->schedule([this, executor](Status status) {
        if (!status.isOK()) {
            return;
        }

        if (_runTask()) {
            schedule(executor);
        }
    });
}

bool ReshardingOplogFetcher::_runTask() {
    auto opCtxRaii = _client->makeOperationContext();
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

    StatusWith<HostAndPort> swTargettedDonor =
        swDonor.getValue()->getTargeter()->findHost(opCtxRaii.get(), readPref);
    if (!swTargettedDonor.isOK()) {
        LOGV2_WARNING(5127202,
                      "Error targetting donor, retrying.",
                      "error"_attr = swTargettedDonor.getStatus());
        return true;
    }

    DBClientConnection donorConn;
    if (auto status = donorConn.connect(swTargettedDonor.getValue(), "ReshardingOplogFetching"_sd);
        !status.isOK()) {
        LOGV2_WARNING(5127201, "Failed connecting to donor, retrying.", "error"_attr = status);
        return true;
    }
    // Reset the OpCtx so consuming can manage short-lived OpCtx lifetimes with the current client.
    opCtxRaii.reset();

    try {
        // Consume will throw if there's oplog entries to be copied. It only returns cleanly when
        // the final oplog has been seen and copied.
        consume(&donorConn);
        return false;
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        _isAlive.store(false);
        return false;
    } catch (const DBException&) {
        LOGV2_WARNING(
            5127200, "Error while fetching, retrying.", "error"_attr = exceptionToStatus());
        return true;
    }
}

boost::optional<ReshardingDonorOplogId> ReshardingOplogFetcher::iterate(
    OperationContext* opCtx,
    DBClientBase* conn,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const ReshardingDonorOplogId startAfter,
    const UUID collUUID,
    const ShardId& recipientShard,
    const bool doesDonorOwnMinKeyChunk,
    const NamespaceString toWriteToNss) {
    // This method will use the input opCtx to perform writes into `toWriteToNss`.
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Create the destination collection if necessary.
    writeConflictRetry(opCtx, "createReshardingLocalOplogBuffer", toWriteToNss.toString(), [&] {
        const CollectionPtr toWriteTo =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toWriteToNss);
        if (toWriteTo) {
            return;
        }

        WriteUnitOfWork wuow(opCtx);
        AutoGetOrCreateDb db(opCtx, toWriteToNss.db(), LockMode::MODE_IX);
        Lock::CollectionLock collLock(opCtx, toWriteToNss, MODE_IX);
        db.getDb()->createCollection(opCtx, toWriteToNss);
        wuow.commit();
    });

    std::vector<BSONObj> serializedPipeline =
        createOplogFetchingPipelineForResharding(
            expCtx, startAfter, collUUID, recipientShard, doesDonorOwnMinKeyChunk)
            ->serializeToBson();

    AggregationRequest aggRequest(NamespaceString::kRsOplogNamespace, serializedPipeline);
    auto readConcernArgs = repl::ReadConcernArgs(
        boost::optional<LogicalTime>(startAfter.getTs()),
        boost::optional<repl::ReadConcernLevel>(repl::ReadConcernLevel::kMajorityReadConcern));
    aggRequest.setReadConcern(readConcernArgs.toBSONInner());
    aggRequest.setHint(BSON("$natural" << 1));

    const bool secondaryOk = true;
    const bool useExhaust = true;
    std::unique_ptr<DBClientCursor> cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        conn, std::move(aggRequest), secondaryOk, useExhaust));

    // Noting some possible optimizations:
    //
    // * Batch more inserts into larger storage transactions.
    // * Parallize writing documents across multiple threads.
    // * Doing either of the above while still using the underlying message buffer of bson objects.
    AutoGetCollection toWriteTo(opCtx, toWriteToNss, LockMode::MODE_IX);
    ReshardingDonorOplogId lastSeen = startAfter;
    while (cursor->more() && _isAlive.load()) {
        WriteUnitOfWork wuow(opCtx);
        BSONObj obj = cursor->next();
        auto nextOplog = uassertStatusOK(repl::OplogEntry::parse(obj));

        lastSeen = ReshardingDonorOplogId::parse({"OplogFetcherParsing"},
                                                 nextOplog.get_id()->getDocument().toBson());
        uassertStatusOK(toWriteTo->insertDocument(opCtx, InsertStatement{obj}, nullptr));
        wuow.commit();
        ++_numOplogEntriesCopied;

        if (isFinalOplog(nextOplog, _reshardingUUID)) {
            return boost::none;
        }
    }

    return lastSeen;
}
}  // namespace mongo
