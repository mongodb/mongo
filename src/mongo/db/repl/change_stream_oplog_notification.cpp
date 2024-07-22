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

#include <boost/move/utility_core.hpp>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

void insertOplogEntry(OperationContext* opCtx,
                      repl::MutableOplogEntry&& oplogEntry,
                      StringData opStr) {
    writeConflictRetry(opCtx, opStr, NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
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

    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    cmdBuilder.append(opName, nssStr);
    cmdBuilder.appendElements(cmd);

    BSONObj fullCmd = cmdBuilder.obj();

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setObject(BSON("msg" << BSON(opName << nssStr)));
    oplogEntry.setObject2(fullCmd);
    oplogEntry.setOpTime(repl::OpTime());
    oplogEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());

    insertOplogEntry(opCtx, std::move(oplogEntry), "ShardCollectionWritesOplog");
}

void notifyChangeStreamsOnDatabaseAdded(OperationContext* opCtx,
                                        const DatabasesAdded& databasesAddedNotification) {
    const std::string operationName = [&] {
        switch (databasesAddedNotification.getPhase()) {
            case CommitPhaseEnum::kSuccessful:
                return "createDatabase";
            case CommitPhaseEnum::kAborted:
                return "createDatabaseAbort";
            case CommitPhaseEnum::kPrepare:
                return "createDatabasePrepare";
            default:
                MONGO_UNREACHABLE;
        }
    }();

    for (const auto& dbName : databasesAddedNotification.getNames()) {
        repl::MutableOplogEntry oplogEntry;
        const auto dbNameStr =
            DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(NamespaceString(dbName));
        oplogEntry.setTid(dbName.tenantId());
        oplogEntry.setObject(BSON("msg" << BSON(operationName << dbNameStr)));
        BSONObjBuilder o2Builder;
        o2Builder.append(operationName, dbNameStr);
        if (databasesAddedNotification.getPhase() == CommitPhaseEnum::kPrepare) {
            o2Builder.append("primaryShard", *databasesAddedNotification.getPrimaryShard());
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
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(NamespaceString(dbName));
    oplogEntry.setTid(dbName.tenantId());
    oplogEntry.setObject(BSON("msg" << BSON("movePrimary" << dbNameStr)));
    oplogEntry.setObject2(
        BSON("movePrimary" << dbNameStr << "from" << oldPrimary << "to" << newPrimary));
    oplogEntry.setOpTime(repl::OpTime());
    oplogEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());

    insertOplogEntry(opCtx, std::move(oplogEntry), "MovePrimaryWritesOplog");
}

void notifyChangeStreamsOnReshardCollectionComplete(OperationContext* opCtx,
                                                    const CollectionResharded& notification) {
    auto buildOpEntry = [&](const std::vector<TagsType>& zones) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(notification.getNss());
        oplogEntry.setTid(notification.getNss().tenantId());
        oplogEntry.setUuid(notification.getSourceUUID());
        const auto nss = NamespaceStringUtil::serialize(notification.getNss(),
                                                        SerializationContext::stateDefault());
        {
            const std::string oMessage = str::stream()
                << "Reshard collection " << nss << " with shard key "
                << notification.getReshardingKey().toString();
            oplogEntry.setObject(BSON("msg" << oMessage));
        }

        {
            BSONObjBuilder o2Builder;
            o2Builder.append("reshardCollection", nss);
            notification.getReshardingUUID().appendToBuilder(&o2Builder, "reshardUUID");
            o2Builder.append("shardKey", notification.getReshardingKey());
            if (notification.getSourceKey()) {
                o2Builder.append("oldShardKey", notification.getSourceKey().value());
            }

            o2Builder.append("unique", notification.getUnique().get_value_or(false));
            if (notification.getNumInitialChunks()) {
                o2Builder.append("numInitialChunks", notification.getNumInitialChunks().value());
            }

            if (notification.getCollation()) {
                o2Builder.append("collation", notification.getCollation().value());
            }

            if (!zones.empty()) {
                BSONArrayBuilder zonesBSON(o2Builder.subarrayStart("zones"));
                for (const auto& zone : zones) {
                    const auto obj = BSON("zone" << zone.getTag() << "min" << zone.getMinKey()
                                                 << "max" << zone.getMaxKey());
                    zonesBSON.append(obj);
                }
                zonesBSON.doneFast();
            }
            oplogEntry.setObject2(o2Builder.obj());
        }

        oplogEntry.setOpTime(repl::OpTime());
        oplogEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
        return oplogEntry;
    };

    // The 'zones' field may be big enough to make the op entry break the 16MB size limit.
    // If such error is detected on serialization time, the insertion gets re-attempted with a
    // redacted version.
    try {
        auto catalogClient = Grid::get(opCtx)->catalogClient();
        const auto zones =
            uassertStatusOK(catalogClient->getTagsForCollection(opCtx, notification.getNss()));
        auto oplogEntry = buildOpEntry(zones);
        insertOplogEntry(opCtx, std::move(oplogEntry), "ReshardCollectionWritesOplog");
    } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
        auto oplogEntry = buildOpEntry({});
        insertOplogEntry(opCtx, std::move(oplogEntry), "ReshardCollectionWritesOplog");
    }
}

void notifyChangeStreamOnEndOfTransaction(OperationContext* opCtx,
                                          const LogicalSessionId& lsid,
                                          const TxnNumber& txnNumber,
                                          const std::vector<NamespaceString>& affectedNamespaces) {
    repl::MutableOplogEntry oplogEntry = change_stream::createEndOfTransactionOplogEntry(
        lsid,
        txnNumber,
        affectedNamespaces,
        repl::OpTime().getTimestamp(),
        opCtx->getServiceContext()->getFastClockSource()->now());
    insertOplogEntry(opCtx, std::move(oplogEntry), "EndOfTransactionWritesOplog");
}

}  // namespace mongo
