/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <system_error>

namespace mongo {
namespace {


class MigrationDestinationManagerTest : public ShardServerTestFixture {
protected:
    /**
     * Instantiates a BSON object in which both "_id" and "X" are set to value.
     */
    static BSONObj createDocument(int value) {
        return BSON("_id" << value << "X" << value);
    }

    /**
     * Creates a list of documents to clone.
     */
    static std::vector<BSONObj> createDocumentsToClone() {
        return {createDocument(1), createDocument(2), createDocument(3)};
    }

    /**
     * Creates a list of documents to clone and converts it to a BSONArray.
     */
    static BSONArray createDocumentsToCloneArray() {
        BSONArrayBuilder arrayBuilder;
        for (auto& doc : createDocumentsToClone()) {
            arrayBuilder.append(doc);
        }
        return arrayBuilder.arr();
    }
};

// Tests that documents will ferry from the fetch logic to the insert logic successfully.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsFromDonorWorksCorrectly) {
    bool ranOnce = false;

    auto fetchBatchFn = [&](OperationContext* opCtx, BSONObj* nextBatch) {
        BSONObjBuilder fetchBatchResultBuilder;

        if (ranOnce) {
            fetchBatchResultBuilder.append("objects", BSONObj());
        } else {
            ranOnce = true;
            fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());
        }

        *nextBatch = fetchBatchResultBuilder.obj();
        return nextBatch->getField("objects").Obj().isEmpty();
    };

    std::vector<BSONObj> resultDocs;

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        auto arr = docs["objects"].Obj();
        if (arr.isEmpty())
            return false;
        for (auto&& docToClone : arr) {
            resultDocs.push_back(docToClone.Obj().getOwned());
        }
        return true;
    };

    MigrationDestinationManager::fetchAndApplyBatch(
        operationContext(), insertBatchFn, fetchBatchFn);

    std::vector<BSONObj> originalDocs = createDocumentsToClone();

    ASSERT_EQ(originalDocs.size(), resultDocs.size());

    for (auto originalDocsIt = originalDocs.begin(), resultDocsIt = resultDocs.begin();
         originalDocsIt != originalDocs.end() && resultDocsIt != resultDocs.end();
         ++originalDocsIt, ++resultDocsIt) {
        ASSERT_BSONOBJ_EQ(*originalDocsIt, *resultDocsIt);
    }
}

// Tests that an exception in the fetch logic will successfully throw an exception on the main
// thread.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsThrowsFetchErrors) {
    bool ranOnce = false;

    auto fetchBatchFn = [&](OperationContext* opCtx, BSONObj* nextBatch) {
        BSONObjBuilder fetchBatchResultBuilder;

        if (ranOnce) {
            uasserted(ErrorCodes::NetworkTimeout, "network error");
        }

        ranOnce = true;
        fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());

        *nextBatch = fetchBatchResultBuilder.obj();
        return nextBatch->getField("objects").Obj().isEmpty();
    };

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        return true;
    };

    ASSERT_THROWS_CODE_AND_WHAT(MigrationDestinationManager::fetchAndApplyBatch(
                                    operationContext(), insertBatchFn, fetchBatchFn),
                                DBException,
                                ErrorCodes::NetworkTimeout,
                                "network error");
}

// Tests that an exception in the insertion logic will successfully throw an exception on the
// main thread.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsCatchesInsertErrors) {
    auto fetchBatchFn = [&](OperationContext* opCtx, BSONObj* nextBatch) {
        BSONObjBuilder fetchBatchResultBuilder;
        fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());
        *nextBatch = fetchBatchResultBuilder.obj();
        return nextBatch->getField("objects").Obj().isEmpty();
    };

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        uasserted(ErrorCodes::FailedToParse, "insertion error");
        return false;
    };

    // Since the error is thrown on another thread, the message becomes "operation was interrupted"
    // on the main thread.

    ASSERT_THROWS_CODE_AND_WHAT(MigrationDestinationManager::fetchAndApplyBatch(
                                    operationContext(), insertBatchFn, fetchBatchFn),
                                DBException,
                                51008,
                                "operation was interrupted");

    ASSERT_EQ(operationContext()->getKillStatus(), 51008);
}

using MigrationDestinationManagerNetworkTest = RouterCatalogCacheTestFixture;

// Verifies MigrationDestinationManager::getCollectionOptions() and
// MigrationDestinationManager::getCollectionIndexes() won't use shard/db versioning without a chunk
// manager and won't include a read concern without afterClusterTime.
TEST_F(MigrationDestinationManagerNetworkTest,
       MigrationDestinationManagerGetIndexesAndCollectionsNoVersionsOrReadConcern) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("db.foo");

    // Shard nss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1" respectively.
    // ShardId("1") is the primary shard for the database.
    auto shards = setupNShards(2);
    auto cm = loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        nss, BSON("_id" << 1), boost::optional<std::string>("1"));

    auto future = launchAsync([&] {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldName(), "listCollections"_sd);
            ASSERT_EQUALS(request.target, HostAndPort("Host0:12345"));
            ASSERT_FALSE(request.cmdObj.hasField("readConcern"));
            ASSERT_FALSE(request.cmdObj.hasField("databaseVersion"));
            ASSERT_BSONOBJ_EQ(request.cmdObj["filter"].Obj(), BSON("name" << nss.coll()));

            const std::vector<BSONObj> colls = {
                BSON("name" << nss.coll() << "options" << BSONObj() << "info"
                            << BSON("readOnly" << false << "uuid" << UUID::gen()) << "idIndex"
                            << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                        << IndexConstants::kIdIndexName))};

            std::string listCollectionsNs = str::stream()
                << nss.db_forTest() << "$cmd.listCollections";
            return BSON(
                "ok" << 1 << "cursor"
                     << BSON("id" << 0LL << "ns" << listCollectionsNs << "firstBatch" << colls));
        });
    });

    MigrationDestinationManager::getCollectionOptions(
        operationContext(), nss, ShardId("0"), boost::none, boost::none);

    future.default_timed_get();

    future = launchAsync([&] {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldName(), "listIndexes"_sd);
            ASSERT_EQUALS(request.target, HostAndPort("Host0:12345"));
            ASSERT_FALSE(request.cmdObj.hasField("readConcern"));
            ASSERT_FALSE(request.cmdObj.hasField("shardVersion"));

            const std::vector<BSONObj> indexes = {BSON(
                "v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName)};
            return BSON(
                "ok" << 1 << "cursor"
                     << BSON("id" << 0LL << "ns" << nss.ns_forTest() << "firstBatch" << indexes));
        });
    });

    MigrationDestinationManager::getCollectionIndexes(
        operationContext(), nss, ShardId("0"), boost::none, boost::none);
    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
