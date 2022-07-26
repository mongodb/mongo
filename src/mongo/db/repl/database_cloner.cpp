/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/repl/database_cloner.h"
#include "mongo/db/repl/database_cloner_common.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync


namespace mongo {
namespace repl {

DatabaseCloner::DatabaseCloner(const std::string& dbName,
                               InitialSyncSharedData* sharedData,
                               const HostAndPort& source,
                               DBClientConnection* client,
                               StorageInterface* storageInterface,
                               ThreadPool* dbPool)
    : InitialSyncBaseCloner(
          "DatabaseCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _dbName(dbName),
      _listCollectionsStage("listCollections", this, &DatabaseCloner::listCollectionsStage) {
    invariant(!dbName.empty());
    _stats.dbname = dbName;
}

BaseCloner::ClonerStages DatabaseCloner::getStages() {
    return {&_listCollectionsStage};
}

void DatabaseCloner::preStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();
}

BaseCloner::AfterStageBehavior DatabaseCloner::listCollectionsStage() {
    BSONObj res;
    auto collectionInfos =
        getClient()->getCollectionInfos(_dbName, ListCollectionsFilter::makeTypeCollectionFilter());

    stdx::unordered_set<std::string> seen;
    for (auto&& info : collectionInfos) {
        ListCollectionResult result;
        try {
            result = ListCollectionResult::parse(
                IDLParserContext("DatabaseCloner::listCollectionsStage"), info);
        } catch (const DBException& e) {
            uasserted(
                ErrorCodes::FailedToParse,
                e.toStatus()
                    .withContext(str::stream() << "Collection info could not be parsed : " << info)
                    .reason());
        }
        NamespaceString collectionNamespace(_dbName, result.getName());
        if (collectionNamespace.isSystem() && !collectionNamespace.isReplicated()) {
            LOGV2_DEBUG(21146,
                        1,
                        "Skipping 'system' collection: {namespace}",
                        "Database cloner skipping 'system' collection",
                        "namespace"_attr = collectionNamespace.ns());
            continue;
        }
        LOGV2_DEBUG(21147,
                    2,
                    "Allowing cloning of collectionInfo: {info}",
                    "Allowing cloning of collectionInfo",
                    "info"_attr = info);

        bool isDuplicate = seen.insert(result.getName().toString()).second;
        uassert(51005,
                str::stream() << "collection info contains duplicate collection name "
                              << "'" << result.getName() << "': " << info,
                isDuplicate);

        // While UUID is a member of CollectionOptions, listCollections does not return the
        // collectionUUID there as part of the options, but instead places it in the 'info' field.
        // We need to move it back to CollectionOptions to create the collection properly.
        result.getOptions().uuid = result.getInfo().getUuid();
        _collections.emplace_back(collectionNamespace, result.getOptions());
    }
    return kContinueNormally;
}

bool DatabaseCloner::isMyFailPoint(const BSONObj& data) const {
    return data["database"].str() == _dbName && BaseCloner::isMyFailPoint(data);
}

void DatabaseCloner::postStage() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.collections = _collections.size();
        _stats.collectionStats.reserve(_collections.size());
        for (const auto& coll : _collections) {
            _stats.collectionStats.emplace_back();
            _stats.collectionStats.back().ns = coll.first.ns();
        }
    }
    for (const auto& coll : _collections) {
        auto& sourceNss = coll.first;
        auto& collectionOptions = coll.second;
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _currentCollectionCloner = std::make_unique<CollectionCloner>(sourceNss,
                                                                          collectionOptions,
                                                                          getSharedData(),
                                                                          getSource(),
                                                                          getClient(),
                                                                          getStorageInterface(),
                                                                          getDBPool());
        }
        auto collStatus = _currentCollectionCloner->run();
        if (collStatus.isOK()) {
            LOGV2_DEBUG(21148,
                        1,
                        "collection clone finished: {namespace}",
                        "Collection clone finished",
                        "namespace"_attr = sourceNss);
        } else {
            LOGV2_ERROR(21149,
                        "collection clone for '{namespace}' failed due to {error}",
                        "Collection clone failed",
                        "namespace"_attr = sourceNss,
                        "error"_attr = collStatus.toString());
            setSyncFailedStatus({ErrorCodes::InitialSyncFailure,
                                 collStatus
                                     .withContext(str::stream() << "Error cloning collection '"
                                                                << sourceNss.toString() << "'")
                                     .toString()});
        }
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _stats.collectionStats[_stats.clonedCollections] = _currentCollectionCloner->getStats();
            _currentCollectionCloner = nullptr;
            // Abort the database cloner if the collection clone failed.
            if (!collStatus.isOK())
                return;
            _stats.clonedCollections++;
        }
    }
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.end = getSharedData()->getClock()->now();
}

DatabaseCloner::Stats DatabaseCloner::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    DatabaseCloner::Stats stats = _stats;
    if (_currentCollectionCloner) {
        stats.collectionStats[_stats.clonedCollections] = _currentCollectionCloner->getStats();
    }
    return stats;
}

std::string DatabaseCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj DatabaseCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.append("dbname", dbname);
    append(&bob);
    return bob.obj();
}

void DatabaseCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("collections", static_cast<long long>(collections));
    builder->appendNumber("clonedCollections", static_cast<long long>(clonedCollections));
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }

    for (auto&& collection : collectionStats) {
        BSONObjBuilder collectionBuilder(builder->subobjStart(collection.ns));
        collection.append(&collectionBuilder);
        collectionBuilder.doneFast();
    }
}

}  // namespace repl
}  // namespace mongo
