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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/str.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
void insertNotificationOplogEntries(OperationContext* opCtx,
                                    std::vector<repl::MutableOplogEntry>&& oplogEntries,
                                    StringData opStr) {
    writeConflictRetry(opCtx, opStr, NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork wunit(opCtx);
        for (auto& oplogEntry : oplogEntries) {
            const auto& oplogOpTime = repl::logOp(opCtx, &oplogEntry);
            uassert(8423339,
                    str::stream() << "Failed to create new oplog entry for oplog with opTime: "
                                  << oplogEntry.getOpTime().toString() << ": "
                                  << redact(oplogEntry.toBSON()),
                    !oplogOpTime.isNull() || !oplogEntry.getNss().isReplicated());
        }
        wunit.commit();
    });
}

}  // namespace

void notifyChangeStreamsOnShardCollection(OperationContext* opCtx,
                                          const CollectionSharded& notification) {
    BSONObjBuilder cmdBuilder;
    StringData opName("shardCollection");

    const auto nssStr =
        NamespaceStringUtil::serialize(notification.getNss(), SerializationContext::stateDefault());
    cmdBuilder.append(opName, nssStr);
    cmdBuilder.appendElements(notification.getRequest());

    BSONObj fullCmd = cmdBuilder.obj();

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(notification.getNss());
    oplogEntry.setUuid(notification.getUuid());
    oplogEntry.setTid(notification.getNss().tenantId());
    oplogEntry.setObject(BSON("msg" << BSON(opName << nssStr)));
    oplogEntry.setObject2(fullCmd);
    oplogEntry.setOpTime(repl::OpTime());
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

    insertNotificationOplogEntries(opCtx, {std::move(oplogEntry)}, "ShardCollectionWritesOplog");
}

repl::MutableOplogEntry buildMovePrimaryOplogEntry(OperationContext* opCtx,
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
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

    return oplogEntry;
}

void notifyChangeStreamsOnMovePrimary(OperationContext* opCtx,
                                      const DatabaseName& dbName,
                                      const ShardId& oldPrimary,
                                      const ShardId& newPrimary) {
    insertNotificationOplogEntries(
        opCtx,
        {buildMovePrimaryOplogEntry(opCtx, dbName, oldPrimary, newPrimary)},
        "MovePrimaryWritesOplog");
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

            if (notification.getProvenance().has_value()) {
                o2Builder.append(
                    "provenance",
                    ReshardingProvenance_serializer(notification.getProvenance().value()));
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
        oplogEntry.setWallClockTime(opCtx->fastClockSource().now());
        return oplogEntry;
    };

    // The 'zones' field may be big enough to make the op entry break the 16MB size limit.
    // If such error is detected on serialization time, the insertion gets re-attempted with a
    // redacted version.
    try {
        auto catalogClient = Grid::get(opCtx)->catalogClient();
        // Due to the size constraints mentioned above, the zone list is not embedded into this
        // notification and it needs to be fetched from the config database.
        const auto& collectionCurrentlyHoldingZones =
            notification.getReferenceToZoneList().value_or(notification.getNss());
        const auto zones = uassertStatusOK(
            catalogClient->getTagsForCollection(opCtx, collectionCurrentlyHoldingZones));
        auto oplogEntry = buildOpEntry(zones);
        insertNotificationOplogEntries(
            opCtx, {std::move(oplogEntry)}, "ReshardCollectionWritesOplog");
    } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
        auto oplogEntry = buildOpEntry({});
        insertNotificationOplogEntries(
            opCtx, {std::move(oplogEntry)}, "ReshardCollectionWritesOplog");
    }
}

repl::MutableOplogEntry buildNamespacePlacementChangedOplogEntry(
    OperationContext* opCtx, const NamespacePlacementChanged& notification) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(notification.getNss());
    oplogEntry.setTid(notification.getNss().tenantId());

    oplogEntry.setObject(
        BSON("msg" << BSON("namespacePlacementChanged" << NamespaceStringUtil::serialize(
                               notification.getNss(), SerializationContext::stateDefault()))));

    const auto buildO2Field = [&] {
        BSONObjBuilder nsFieldBuilder;
        if (notification.getNss() != NamespaceString::kEmpty) {
            nsFieldBuilder.append("db", notification.getNss().dbName().toStringForResourceId());

            if (!notification.getNss().isDbOnly()) {
                nsFieldBuilder.append("coll", notification.getNss().coll());
            }
        }

        return BSON("namespacePlacementChanged" << 1 << "ns" << nsFieldBuilder.obj()
                                                << "committedAt" << notification.getCommittedAt());
    };

    oplogEntry.setObject2(buildO2Field());

    oplogEntry.setOpTime(repl::OpTime());
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

    return oplogEntry;
}

void notifyChangeStreamsOnNamespacePlacementChanged(OperationContext* opCtx,
                                                    const NamespacePlacementChanged& notification) {
    insertNotificationOplogEntries(opCtx,
                                   {buildNamespacePlacementChangedOplogEntry(opCtx, notification)},
                                   "NamespacePlacementChangedWritesOplog");
}

void notifyChangeStreamOnEndOfTransaction(OperationContext* opCtx,
                                          const LogicalSessionId& lsid,
                                          const TxnNumber& txnNumber,
                                          const std::vector<NamespaceString>& affectedNamespaces) {
    repl::MutableOplogEntry oplogEntry =
        change_stream::createEndOfTransactionOplogEntry(lsid,
                                                        txnNumber,
                                                        affectedNamespaces,
                                                        repl::OpTime().getTimestamp(),
                                                        opCtx->fastClockSource().now());
    insertNotificationOplogEntries(opCtx, {std::move(oplogEntry)}, "EndOfTransactionWritesOplog");
}

std::vector<repl::MutableOplogEntry> buildMoveChunkOplogEntries(
    OperationContext* opCtx,
    const NamespaceString& collName,
    const boost::optional<UUID>& collUUID,
    const ShardId& donor,
    const ShardId& recipient,
    bool noMoreCollectionChunksOnDonor,
    bool firstCollectionChunkOnRecipient) {
    const auto nss = NamespaceStringUtil::serialize(collName, SerializationContext::stateDefault());
    std::vector<repl::MutableOplogEntry> oplogEntries;
    {
        repl::MutableOplogEntry oplogEntry;
        StringData opName("moveChunk");

        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(collName);
        oplogEntry.setUuid(collUUID);
        oplogEntry.setTid(collName.tenantId());
        oplogEntry.setObject(BSON("msg" << BSON(opName << nss)));
        oplogEntry.setObject2(BSON(opName << nss << "donor" << donor << "recipient" << recipient
                                          << "allCollectionChunksMigratedFromDonor"
                                          << noMoreCollectionChunksOnDonor));
        oplogEntry.setOpTime(repl::OpTime());
        oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

        oplogEntries.push_back(std::move(oplogEntry));
    }

    // Conditionally emit the legacy 'migrateLastChunkFromShard' and 'migrateChunkToNewShard' op
    // entry types, consumed by V1 change stream readers.
    if (noMoreCollectionChunksOnDonor) {
        repl::MutableOplogEntry legacyOplogEntry;

        legacyOplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        legacyOplogEntry.setNss(collName);
        legacyOplogEntry.setUuid(collUUID);
        legacyOplogEntry.setTid(collName.tenantId());
        const std::string oMessage = str::stream()
            << "Migrate the last chunk for " << collName.toStringForErrorMsg() << " off shard "
            << donor;
        legacyOplogEntry.setObject(BSON("msg" << oMessage));
        legacyOplogEntry.setObject2(BSON("migrateLastChunkFromShard" << nss << "shardId" << donor));
        legacyOplogEntry.setOpTime(repl::OpTime());
        legacyOplogEntry.setWallClockTime(opCtx->fastClockSource().now());

        oplogEntries.push_back(std::move(legacyOplogEntry));
    }

    if (firstCollectionChunkOnRecipient) {
        repl::MutableOplogEntry legacyOplogEntry;
        legacyOplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        legacyOplogEntry.setNss(collName);
        legacyOplogEntry.setUuid(collUUID);
        legacyOplogEntry.setTid(collName.tenantId());
        const std::string oMessage = str::stream()
            << "Migrating chunk from shard " << donor << " to shard " << recipient
            << " with no chunks for this collection";
        legacyOplogEntry.setObject(BSON("msg" << oMessage));
        legacyOplogEntry.setObject2(BSON("migrateChunkToNewShard" << nss << "fromShardId" << donor
                                                                  << "toShardId" << recipient));
        legacyOplogEntry.setOpTime(repl::OpTime());
        legacyOplogEntry.setWallClockTime(opCtx->fastClockSource().now());

        oplogEntries.push_back(std::move(legacyOplogEntry));
    }

    return oplogEntries;
}

void notifyChangeStreamsOnChunkMigrated(OperationContext* opCtx,
                                        const NamespaceString& collName,
                                        const boost::optional<UUID>& collUUID,
                                        const ShardId& donor,
                                        const ShardId& recipient,
                                        bool noMoreCollectionChunksOnDonor,
                                        bool firstCollectionChunkOnRecipient) {
    insertNotificationOplogEntries(opCtx,
                                   buildMoveChunkOplogEntries(opCtx,
                                                              collName,
                                                              collUUID,
                                                              donor,
                                                              recipient,
                                                              noMoreCollectionChunksOnDonor,
                                                              firstCollectionChunkOnRecipient),
                                   "ChunkMigrationWritesOplog");
}

}  // namespace mongo
