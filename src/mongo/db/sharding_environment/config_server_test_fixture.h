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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

/**
 * Takes two arrays of BSON objects and asserts that they contain the same documents
 */
inline void assertBSONObjsSame(const std::vector<BSONObj>& expectedBSON,
                               const std::vector<BSONObj>& foundBSON) {
    ASSERT_EQUALS(expectedBSON.size(), foundBSON.size());

    auto flags =
        BSONObj::ComparisonRules::kIgnoreFieldOrder | BSONObj::ComparisonRules::kConsiderFieldName;

    for (const auto& expectedObj : expectedBSON) {
        bool wasFound = false;
        for (const auto& foundObj : foundBSON) {
            if (expectedObj.woCompare(foundObj, {}, flags) == 0) {
                wasFound = true;
                break;
            }
        }
        ASSERT_TRUE(wasFound);
    }
}

/**
 * Provides config-specific functionality in addition to the mock storage engine and mock network
 * provided by ShardingMongoDTestFixture.
 */
class ConfigServerTestFixture : public ShardingMongoDTestFixture {
protected:
    explicit ConfigServerTestFixture(Options options = {}, bool setUpMajorityReads = true);
    ~ConfigServerTestFixture() override;

    void setUp() override;
    void tearDown() override;

    std::shared_ptr<Shard> getConfigShard() const;

    /**
     * Insert a document to this config server to the specified namespace.
     */
    Status insertToConfigCollection(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const BSONObj& doc);

    /**
     * Updates a document to this config server to the specified namespace.
     */
    Status updateToConfigCollection(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const BSONObj& query,
                                    const BSONObj& update,
                                    bool upsert);

    /**
     * Deletes a document to this config server to the specified namespace.
     */
    Status deleteToConfigCollection(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const BSONObj& doc,
                                    bool multi);

    /**
     * Reads a single document from a collection living on the config server.
     */
    StatusWith<BSONObj> findOneOnConfigCollection(OperationContext* opCtx,
                                                  const NamespaceString& ns,
                                                  const BSONObj& filter,
                                                  const BSONObj& sort = {});
    /**
     * Reads a single document from a collection living on the config server and parses it into the
     * specified type.
     * Note: T must be a valid IDL type or any type that provides a static parse() method as defined
     * for IDL types.
     */
    template <typename T>
    T findOneOnConfigCollection(OperationContext* opCtx,
                                const NamespaceString& ns,
                                const BSONObj& filter,
                                const BSONObj& sort = {}) {
        auto result = findOneOnConfigCollection(opCtx, ns, filter, sort);
        uassertStatusOK(result.getStatus());

        IDLParserContext ctx("");
        return T::parse(result.getValue(), ctx);
    }

    /**
     * Setup the config.shards collection to contain the given shards.
     */
    virtual void setupShards(const std::vector<ShardType>& shards);

    /**
     * Retrieves the shard document from the config server.
     * Returns {ErrorCodes::ShardNotFound} if the given shard does not exists.
     */
    StatusWith<ShardType> getShardDoc(OperationContext* opCtx, const std::string& shardId);

    /**
     * Setup the config.chunks collection to contain the given chunks.
     */
    CollectionType setupCollection(
        const NamespaceString& nss,
        const KeyPattern& shardKey,
        const std::vector<ChunkType>& chunks,
        std::function<void(CollectionType& coll)> collectionCustomizer = [](CollectionType& coll) {
        });

    /**
     * Retrieves the chunk document <uuid, minKey> from the config server.
     * This is the recommended way to get a chunk document.
     */
    StatusWith<ChunkType> getChunkDoc(OperationContext* opCtx,
                                      const UUID& uuid,
                                      const BSONObj& minKey,
                                      const OID& collEpoch,
                                      const Timestamp& collTimestamp);

    /**
     * Retrieves the chunk document <minKey> from the config server.
     * This function assumes that there is just one chunk document associated to minKey. This can
     * lead to some problems in scenarios where there are two or more collections that are splitted
     * in the same way.
     */
    StatusWith<ChunkType> getChunkDoc(OperationContext* opCtx,
                                      const BSONObj& minKey,
                                      const OID& collEpoch,
                                      const Timestamp& collTimestamp);

    /**
     * Returns the collection placement version.
     */
    StatusWith<ChunkVersion> getCollectionPlacementVersion(OperationContext* opCtx,
                                                           const NamespaceString& nss);

    /**
     * Inserts a document for the database into the config.databases collection.
     */
    DatabaseType setupDatabase(const DatabaseName& dbName,
                               const ShardId& primaryShard,
                               const DatabaseVersion& dbVersion = DatabaseVersion(UUID::gen(),
                                                                                  Timestamp()));

    /**
     * Returns the indexes definitions defined on a given collection.
     */
    StatusWith<std::vector<BSONObj>> getIndexes(OperationContext* opCtx, const NamespaceString& ns);

    /**
     * Returns the stored raw pointer to the addShard TaskExecutor's NetworkInterface.
     */
    executor::NetworkInterfaceMock* networkForAddShard() const;

    /**
     * Returns the stored raw pointer to the addShard TaskExecutor.
     */
    executor::TaskExecutor* executorForAddShard() const;

    /**
     * Same as ShardingMongoDTestFixture::onCommand but run against _addShardNetworkTestEnv.
     */
    void onCommandForAddShard(executor::NetworkTestEnv::OnCommandFunction func);

    /**
     * Returns all the keys in admin.system.keys
     */
    std::vector<KeysCollectionDocument> getKeys(OperationContext* opCtx);

    /**
     * Sets this node up and initialized the collections and indexes in the config db.
     */
    void setUpAndInitializeConfigDb();

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override;

    std::unique_ptr<ClusterCursorManager> makeClusterCursorManager() override;

    std::unique_ptr<BalancerConfiguration> makeBalancerConfiguration() override;

protected:
    void setupOpObservers() override;

private:
    // Since these are currently private members of the real ShardingCatalogManager, we store a raw
    // pointer to them here.
    executor::NetworkInterfaceMock* _mockNetworkForAddShard;
    executor::TaskExecutor* _executorForAddShard;

    // Allows for processing tasks through the NetworkInterfaceMock/ThreadPoolMock subsystem.
    std::unique_ptr<executor::NetworkTestEnv> _addShardNetworkTestEnv;
};

}  // namespace mongo
