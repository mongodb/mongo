// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/change_stream_reader_builder_impl.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/s/change_streams/all_databases_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/collection_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/s/change_streams/database_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/historical_placement_fetcher_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre_util.h"

#include <fmt/format.h>

namespace mongo {

std::unique_ptr<ChangeStreamShardTargeter> ChangeStreamReaderBuilderImpl::buildShardTargeter(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    auto fetcher = std::make_unique<HistoricalPlacementFetcherImpl>();
    switch (changeStream.getChangeStreamType()) {
        case ChangeStreamType::kCollection: {
            return std::make_unique<CollectionChangeStreamShardTargeterImpl>(std::move(fetcher));
        }
        case ChangeStreamType::kDatabase: {
            return std::make_unique<DatabaseChangeStreamShardTargeterImpl>(std::move(fetcher));
        }
        case ChangeStreamType::kAllDatabases: {
            return std::make_unique<AllDatabasesChangeStreamShardTargeterImpl>(std::move(fetcher));
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
            auto escapedDbName = pcre_util::quoteMeta(dbName);
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

    // Match NamespacePlacementChanged events with empty namespace (FCV upgrade/downgrade).
    BSONObj doesNotExist = BSON("$exists" << false);
    auto placementChangedPredicate =
        BSON("o2.namespacePlacementChanged" << 1 << "o2.ns.db" << doesNotExist << "o2.ns.coll"
                                            << doesNotExist);

    BSONObj databaseCreatedPredicate;
    switch (changeStream.getChangeStreamType()) {
        case ChangeStreamType::kCollection:
            [[fallthrough]];
        case ChangeStreamType::kDatabase: {
            auto dbName = DatabaseNameUtil::serialize(changeStream.getNamespace()->dbName(),
                                                      SerializationContext::stateCommandRequest());
            databaseCreatedPredicate =
                BSON(repl::OplogEntry::kNssFieldName << configDbNssString
                                                     << repl::OplogEntry::kOpTypeFieldName << "i"
                                                     << "o._id" << dbName);
            break;
        }
        case ChangeStreamType::kAllDatabases:
            databaseCreatedPredicate =
                BSON(repl::OplogEntry::kNssFieldName << configDbNssString
                                                     << repl::OplogEntry::kOpTypeFieldName << "i");
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(10719002);
    }

    return BSON("$or" << BSON_ARRAY(databaseCreatedPredicate << placementChangedPredicate));
}

std::set<std::string> ChangeStreamReaderBuilderImpl::getControlEventTypesOnConfigServer(
    OperationContext* opCtx, const ChangeStream& changeStream) {
    return {std::string(DatabaseCreatedControlEvent::opType),
            std::string(NamespacePlacementChangedControlEvent::opType)};
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
