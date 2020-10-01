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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"

namespace mongo {
ReshardingDonorOplogId ReshardingOplogFetcher::iterate(
    OperationContext* opCtx,
    DBClientBase* conn,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    ReshardingDonorOplogId startAfter,
    UUID collUUID,
    const ShardId& recipientShard,
    bool doesDonorOwnMinKeyChunk,
    NamespaceString toWriteToNss) {

    // This method will use the input opCtx to perform writes into `toWriteToNss`.
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Create the destination collection if necessary.
    writeConflictRetry(opCtx, "createReshardingLocalOplogBuffer", toWriteToNss.toString(), [&] {
        const CollectionPtr toWriteTo =
            CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, toWriteToNss);
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
    while (cursor->more()) {
        WriteUnitOfWork wuow(opCtx);
        BSONObj obj = cursor->next();
        lastSeen = ReshardingDonorOplogId::parse({"OplogFetcherParsing"}, obj["_id"].Obj());
        uassertStatusOK(toWriteTo->insertDocument(opCtx, InsertStatement{obj}, nullptr));
        wuow.commit();
    }

    return lastSeen;
}
}  // namespace mongo
