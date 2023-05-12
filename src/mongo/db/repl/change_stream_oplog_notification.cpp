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

#include "mongo/db/repl/change_stream_oplog_notification.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/shard_id.h"

namespace mongo {

namespace {

void insertOplogEntry(OperationContext* opCtx,
                      repl::MutableOplogEntry&& oplogEntry,
                      StringData opStr) {
    writeConflictRetry(opCtx, opStr, NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork wunit(opCtx);
        const auto& oplogOpTime = repl::logOp(opCtx, &oplogEntry);
        uassert(8423339,
                str::stream() << "Failed to create new oplog entry for oplog with opTime: "
                              << oplogEntry.getOpTime().toString() << ": "
                              << redact(oplogEntry.toBSON()),
                !oplogOpTime.isNull());
        wunit.commit();
    });
}

}  // namespace

void notifyChangeStreamsOnShardCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          BSONObj cmd,
                                          CommitPhase commitPhase,
                                          const boost::optional<std::set<ShardId>>& shardIds) {
    BSONObjBuilder cmdBuilder;
    std::string opName;
    switch (commitPhase) {
        case mongo::CommitPhase::kSuccessful:
            opName = "shardCollection";
            break;
        case CommitPhase::kAborted:
            opName = "shardCollectionAbort";
            break;
        case CommitPhase::kPrepare:
            // in case of prepare, shardsIds is required
            cmdBuilder.append("shards", *shardIds);
            opName = "shardCollectionPrepare";
            break;
        default:
            MONGO_UNREACHABLE;
    }

    cmdBuilder.append(opName, nss.ns());
    cmdBuilder.appendElements(cmd);

    BSONObj fullCmd = cmdBuilder.obj();

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setObject(BSON("msg" << BSON(opName << nss.ns())));
    oplogEntry.setObject2(fullCmd);
    oplogEntry.setOpTime(repl::OpTime());
    oplogEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());

    insertOplogEntry(opCtx, std::move(oplogEntry), "ShardCollectionWritesOplog");
}

void notifyChangeStreamsOnDatabaseAdded(OperationContext* opCtx,
                                        const DatabasesAdded& databasesAddedNotification) {
    const auto& notifiedPhase = databasesAddedNotification.getPhase();
    for (const auto& dbName : databasesAddedNotification.getNames()) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(NamespaceString(dbName));
        oplogEntry.setTid(dbName.tenantId());
        oplogEntry.setObject(BSON("msg" << BSON("createDatabase" << dbName.db())));
        BSONObjBuilder o2Builder;
        o2Builder.append("createDatabase", dbName.db());
        if (notifiedPhase) {
            o2Builder.append("phase", *notifiedPhase);
            if (*notifiedPhase == CommitPhaseEnum::kPrepare) {
                o2Builder.append("primaryShard", *databasesAddedNotification.getPrimaryShard());
            }
        }

        o2Builder.append("isImported", databasesAddedNotification.getAreImported());
        oplogEntry.setObject2(o2Builder.obj());
        oplogEntry.setOpTime(repl::OpTime());
        oplogEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());

        insertOplogEntry(opCtx, std::move(oplogEntry), "DbAddedToConfigCatalogWritesOplog");
    }
}

void notifyChangeStreamsOnMovePrimary(OperationContext* opCtx,
                                      const DatabaseName& dbName,
                                      const ShardId& oldPrimary,
                                      const ShardId& newPrimary) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(NamespaceString(dbName));
    oplogEntry.setTid(dbName.tenantId());
    oplogEntry.setObject(BSON("msg" << BSON("movePrimary" << dbName.db())));
    oplogEntry.setObject2(
        BSON("movePrimary" << dbName.db() << "from" << oldPrimary << "to" << newPrimary));
    oplogEntry.setOpTime(repl::OpTime());
    oplogEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());

    insertOplogEntry(opCtx, std::move(oplogEntry), "MovePrimaryWritesOplog");
}

}  // namespace mongo
