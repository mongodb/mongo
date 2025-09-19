/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/change_streams/change_stream_reader_builder_impl.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/s/change_streams/all_databases_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/collection_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/s/change_streams/database_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/historical_placement_fetcher_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"

#include <fmt/format.h>

namespace mongo {

std::unique_ptr<ChangeStreamShardTargeter> ChangeStreamReaderBuilderImpl::buildShardTargeter(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    switch (changeStream.getChangeStreamType()) {
        case ChangeStreamType::kCollection: {
            auto fetcher = std::make_unique<HistoricalPlacementFetcherImpl>();
            return std::make_unique<CollectionChangeStreamShardTargeterImpl>(std::move(fetcher));
        }
        case ChangeStreamType::kDatabase: {
            return std::make_unique<DatabaseChangeStreamShardTargeterImpl>();
        }
        case ChangeStreamType::kAllDatabases: {
            return std::make_unique<AllDatabasesChangeStreamShardTargeterImpl>();
        }
    }

    MONGO_UNREACHABLE_TASSERT(10719000);
}

BSONObj ChangeStreamReaderBuilderImpl::buildControlEventFilterForDataShard(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    const auto& nss = changeStream.getNamespace();

    BSONObj predicateForMoveChunk;
    BSONObj predicateForMovePrimary;
    BSONObj predicateForNamespacePlacementChanged;

    BSONObj doesNotExist = BSON("$exists" << false);

    switch (changeStream.getChangeStreamType()) {
        case ChangeStreamType::kCollection: {
            auto dbName = DatabaseNameUtil::serialize(nss->dbName(),
                                                      SerializationContext::stateCommandRequest());
            auto collName = nss->coll();
            auto nssString =
                NamespaceStringUtil::serialize(*nss, SerializationContext::stateCommandRequest());
            predicateForMoveChunk = BSON("o2.moveChunk" << nssString);
            predicateForMovePrimary = BSON("o2.movePrimary" << dbName);

            const auto nssPredicate = BSON("o2.ns" << BSON("db" << dbName << "coll" << collName));
            const auto dbPredicate = BSON("o2.ns.db" << dbName << "o2.ns.coll" << doesNotExist);
            const auto clusterPredicate =
                BSON("o2.ns.db" << doesNotExist << "o2.ns.coll" << doesNotExist);
            predicateForNamespacePlacementChanged =
                BSON("o2.namespacePlacementChanged"
                     << 1 << "$or" << BSON_ARRAY(nssPredicate << dbPredicate << clusterPredicate));
            break;
        }
        case ChangeStreamType::kDatabase: {
            auto dbName = DatabaseNameUtil::serialize(nss->dbName(),
                                                      SerializationContext::stateCommandRequest());
            auto escapedDbName = change_stream::regexEscapeNsForChangeStream(dbName);
            predicateForMoveChunk =
                BSON("o2.moveChunk" << BSONRegEx(fmt::format("^{}\\.", escapedDbName)));
            predicateForMovePrimary = BSON("o2.movePrimary" << dbName);

            const auto dbPredicate = BSON("o2.ns.db" << dbName);
            const auto clusterPredicate =
                BSON("o2.ns.db" << doesNotExist << "o2.ns.coll" << doesNotExist);
            predicateForNamespacePlacementChanged =
                BSON("o2.namespacePlacementChanged" << 1 << "$or"
                                                    << BSON_ARRAY(dbPredicate << clusterPredicate));
            break;
        }
        case ChangeStreamType::kAllDatabases: {
            predicateForMoveChunk = BSON("o2.moveChunk" << BSON("$exists" << true));
            predicateForMovePrimary = BSON("o2.movePrimary" << BSON("$exists" << true));
            predicateForNamespacePlacementChanged = BSON("o2.namespacePlacementChanged" << 1);
            break;
        }
        default:
            MONGO_UNREACHABLE_TASSERT(10719001);
    }

    return BSON("$or" << BSON_ARRAY(predicateForMoveChunk
                                    << predicateForMovePrimary
                                    << predicateForNamespacePlacementChanged));
}

std::set<std::string> ChangeStreamReaderBuilderImpl::getControlEventTypesOnDataShard(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return {std::string(MovePrimaryControlEvent::opType),
            std::string(MoveChunkControlEvent::opType),
            std::string(NamespacePlacementChangedControlEvent::opType)};
}

BSONObj ChangeStreamReaderBuilderImpl::buildControlEventFilterForConfigServer(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    const auto& configDbNssString = NamespaceStringUtil::serialize(
        NamespaceString::kConfigDatabasesNamespace, SerializationContext::stateCommandRequest());
    switch (changeStream.getChangeStreamType()) {
        case ChangeStreamType::kCollection:
            [[fallthrough]];
        case ChangeStreamType::kDatabase: {
            auto dbName = DatabaseNameUtil::serialize(changeStream.getNamespace()->dbName(),
                                                      SerializationContext::stateCommandRequest());
            return BSON(repl::OplogEntry::kNssFieldName
                        << configDbNssString << repl::OplogEntry::kOpTypeFieldName << "i" << "o._id"
                        << dbName);
        }
        case ChangeStreamType::kAllDatabases:
            return BSON(repl::OplogEntry::kNssFieldName
                        << configDbNssString << repl::OplogEntry::kOpTypeFieldName << "i");
    }

    MONGO_UNREACHABLE_TASSERT(10719002);
}

std::set<std::string> ChangeStreamReaderBuilderImpl::getControlEventTypesOnConfigServer(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return {std::string(DatabaseCreatedControlEvent::opType)};
}

namespace {
ServiceContext::ConstructorActionRegisterer changeStreamReaderBuilderRegisterer(
    "ChangeStreamReaderBuilder",
    {},
    [](ServiceContext* serviceContext) {
        invariant(serviceContext);
        ChangeStreamReaderBuilder::set(serviceContext,
                                       std::make_unique<ChangeStreamReaderBuilderImpl>());
    },
    {});
}  // namespace

}  // namespace mongo
