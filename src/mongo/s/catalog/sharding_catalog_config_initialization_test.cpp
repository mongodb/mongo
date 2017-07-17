/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::string;
using std::vector;
using unittest::assertGet;

/**
 * Takes two arrays of BSON objects and asserts that they contain the same documents
 */
void assertBSONObjsSame(const std::vector<BSONObj>& expectedBSON,
                        const std::vector<BSONObj>& foundBSON) {
    ASSERT_EQUALS(expectedBSON.size(), foundBSON.size());

    for (const auto& expectedObj : expectedBSON) {
        bool wasFound = false;
        for (const auto& foundObj : foundBSON) {
            if (expectedObj.woCompare(foundObj) == 0) {
                wasFound = true;
                break;
            }
        }
        ASSERT_TRUE(wasFound);
    }
}

using ConfigInitializationTest = ConfigServerTestFixture;

TEST_F(ConfigInitializationTest, UpgradeNotNeeded) {
    VersionType version;
    version.setClusterId(OID::gen());
    version.setCurrentVersion(CURRENT_CONFIG_VERSION);
    version.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), version.toBSON()));

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_EQUALS(version.getClusterId(), foundVersion.getClusterId());
    ASSERT_EQUALS(version.getCurrentVersion(), foundVersion.getCurrentVersion());
    ASSERT_EQUALS(version.getMinCompatibleVersion(), foundVersion.getMinCompatibleVersion());
}

TEST_F(ConfigInitializationTest, InitIncompatibleVersion) {
    VersionType version;
    version.setClusterId(OID::gen());
    version.setCurrentVersion(MIN_COMPATIBLE_CONFIG_VERSION - 1);
    version.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION - 2);
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), version.toBSON()));

    ASSERT_EQ(ErrorCodes::IncompatibleShardingConfigVersion,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_EQUALS(version.getClusterId(), foundVersion.getClusterId());
    ASSERT_EQUALS(version.getCurrentVersion(), foundVersion.getCurrentVersion());
    ASSERT_EQUALS(version.getMinCompatibleVersion(), foundVersion.getMinCompatibleVersion());
}

TEST_F(ConfigInitializationTest, InitClusterMultipleVersionDocs) {
    VersionType version;
    version.setClusterId(OID::gen());
    version.setCurrentVersion(MIN_COMPATIBLE_CONFIG_VERSION - 2);
    version.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION - 3);
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), version.toBSON()));

    ASSERT_OK(insertToConfigCollection(operationContext(),
                                       NamespaceString(VersionType::ConfigNS),
                                       BSON("_id"
                                            << "a second document")));

    ASSERT_EQ(ErrorCodes::TooManyMatchingDocuments,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigInitializationTest, InitInvalidConfigVersionDoc) {
    BSONObj versionDoc(fromjson(R"({
                    _id: 1,
                    minCompatibleVersion: "should be numeric",
                    currentVersion: 7,
                    clusterId: ObjectId("55919cc6dbe86ce7ac056427")
                })"));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), versionDoc));

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}


TEST_F(ConfigInitializationTest, InitNoVersionDocEmptyConfig) {
    // Make sure there is no existing document
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  findOneOnConfigCollection(
                      operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());
    ASSERT_EQUALS(CURRENT_CONFIG_VERSION, foundVersion.getCurrentVersion());
    ASSERT_EQUALS(MIN_COMPATIBLE_CONFIG_VERSION, foundVersion.getMinCompatibleVersion());
}

TEST_F(ConfigInitializationTest, InitVersionTooHigh) {
    VersionType version;
    version.setClusterId(OID::gen());
    version.setCurrentVersion(10000);
    version.setMinCompatibleVersion(10000);
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), version.toBSON()));

    ASSERT_EQ(ErrorCodes::IncompatibleShardingConfigVersion,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigInitializationTest, OnlyRunsOnce) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());
    ASSERT_EQUALS(CURRENT_CONFIG_VERSION, foundVersion.getCurrentVersion());
    ASSERT_EQUALS(MIN_COMPATIBLE_CONFIG_VERSION, foundVersion.getMinCompatibleVersion());

    ASSERT_EQUALS(ErrorCodes::AlreadyInitialized,
                  ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigInitializationTest, ReRunsIfDocRolledBackThenReElected) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());
    ASSERT_EQUALS(CURRENT_CONFIG_VERSION, foundVersion.getCurrentVersion());
    ASSERT_EQUALS(MIN_COMPATIBLE_CONFIG_VERSION, foundVersion.getMinCompatibleVersion());

    // Now remove the version document and re-run initializeConfigDatabaseIfNeeded().
    {
        // Mirror what happens if the config.version document is rolled back.
        ON_BLOCK_EXIT([&] {
            replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY).ignore();
        });
        ASSERT_OK(replicationCoordinator()->setFollowerMode(repl::MemberState::RS_ROLLBACK));
        auto opCtx = operationContext();
        repl::UnreplicatedWritesBlock uwb(opCtx);
        auto nss = NamespaceString(VersionType::ConfigNS);
        writeConflictRetry(opCtx, "removeConfigDocuments", nss.ns(), [&] {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto coll = autoColl.getCollection();
            ASSERT_TRUE(coll);
            auto cursor = coll->getCursor(opCtx);
            std::vector<RecordId> recordIds;
            while (auto recordId = cursor->next()) {
                recordIds.push_back(recordId->id);
            }
            mongo::WriteUnitOfWork wuow(opCtx);
            for (auto recordId : recordIds) {
                coll->deleteDocument(opCtx, kUninitializedStmtId, recordId, nullptr);
            }
            wuow.commit();
            ASSERT_EQUALS(0UL, coll->numRecords(opCtx));
        });
    }

    // Verify the document was actually removed.
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  findOneOnConfigCollection(
                      operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    // Re-create the config.version document.
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto newVersionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString(VersionType::ConfigNS), BSONObj()));

    VersionType newFoundVersion = assertGet(VersionType::fromBSON(newVersionDoc));

    ASSERT_TRUE(newFoundVersion.getClusterId().isSet());
    ASSERT_NOT_EQUALS(newFoundVersion.getClusterId(), foundVersion.getClusterId());
    ASSERT_EQUALS(CURRENT_CONFIG_VERSION, newFoundVersion.getCurrentVersion());
    ASSERT_EQUALS(MIN_COMPATIBLE_CONFIG_VERSION, newFoundVersion.getMinCompatibleVersion());
}

TEST_F(ConfigInitializationTest, BuildsNecessaryIndexes) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto expectedChunksIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"
                 << "ns"
                 << "config.chunks"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("ns" << 1 << "min" << 1) << "name"
                 << "ns_1_min_1"
                 << "ns"
                 << "config.chunks"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("ns" << 1 << "shard" << 1 << "min" << 1)
                 << "name"
                 << "ns_1_shard_1_min_1"
                 << "ns"
                 << "config.chunks"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("ns" << 1 << "lastmod" << 1) << "name"
                 << "ns_1_lastmod_1"
                 << "ns"
                 << "config.chunks")};
    auto expectedLockpingsIndexes =
        std::vector<BSONObj>{BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                      << "_id_"
                                      << "ns"
                                      << "config.lockpings"),
                             BSON("v" << 2 << "key" << BSON("ping" << 1) << "name"
                                      << "ping_1"
                                      << "ns"
                                      << "config.lockpings")};
    auto expectedLocksIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"
                 << "ns"
                 << "config.locks"),
        BSON("v" << 2 << "key" << BSON("ts" << 1) << "name"
                 << "ts_1"
                 << "ns"
                 << "config.locks"),
        BSON("v" << 2 << "key" << BSON("state" << 1 << "process" << 1) << "name"
                 << "state_1_process_1"
                 << "ns"
                 << "config.locks")};
    auto expectedShardsIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"
                 << "ns"
                 << "config.shards"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("host" << 1) << "name"
                 << "host_1"
                 << "ns"
                 << "config.shards")};
    auto expectedTagsIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"
                 << "ns"
                 << "config.tags"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("ns" << 1 << "min" << 1) << "name"
                 << "ns_1_min_1"
                 << "ns"
                 << "config.tags"),
        BSON("v" << 2 << "key" << BSON("ns" << 1 << "tag" << 1) << "name"
                 << "ns_1_tag_1"
                 << "ns"
                 << "config.tags")};

    auto foundChunksIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString(ChunkType::ConfigNS)));
    assertBSONObjsSame(expectedChunksIndexes, foundChunksIndexes);

    auto foundLockpingsIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString(LockpingsType::ConfigNS)));
    assertBSONObjsSame(expectedLockpingsIndexes, foundLockpingsIndexes);

    auto foundLocksIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString(LocksType::ConfigNS)));
    assertBSONObjsSame(expectedLocksIndexes, foundLocksIndexes);

    auto foundShardsIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString(ShardType::ConfigNS)));
    assertBSONObjsSame(expectedShardsIndexes, foundShardsIndexes);

    auto foundTagsIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString(TagsType::ConfigNS)));
    assertBSONObjsSame(expectedTagsIndexes, foundTagsIndexes);
}

TEST_F(ConfigInitializationTest, CompatibleIndexAlreadyExists) {
    getConfigShard()
        ->createIndexOnConfig(
            operationContext(), NamespaceString(ShardType::ConfigNS), BSON("host" << 1), true)
        .transitional_ignore();

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto expectedShardsIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"
                 << "ns"
                 << "config.shards"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("host" << 1) << "name"
                 << "host_1"
                 << "ns"
                 << "config.shards")};


    auto foundShardsIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString(ShardType::ConfigNS)));
    assertBSONObjsSame(expectedShardsIndexes, foundShardsIndexes);
}

TEST_F(ConfigInitializationTest, IncompatibleIndexAlreadyExists) {
    // Make the index non-unique even though its supposed to be unique, make sure initialization
    // fails
    getConfigShard()
        ->createIndexOnConfig(
            operationContext(), NamespaceString(ShardType::ConfigNS), BSON("host" << 1), false)
        .transitional_ignore();

    ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                  ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));
}

}  // unnamed namespace
}  // namespace mongo
