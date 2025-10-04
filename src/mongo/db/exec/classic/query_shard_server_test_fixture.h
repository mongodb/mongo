/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/client/index_spec.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/shard_role_catalog/metadata_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"

#include <memory>

namespace mongo {
/**
 * A text fixture used to verify the execution patterns of queres on sharded collections.
 */
class QueryShardServerTestFixture : public ShardServerTestFixture {
public:
    /**
     * Helper struct used to specify either a StageState with no output or a BSONObj representing
     * the current output of the stage (corresponding to the ADVANCED state).
     */
    using DoWorkResult = std::variant<PlanStage::StageState, BSONObj>;

    /**
     * Helper validating the execution pattern of the current stage.
     */
    static void doWorkAndValidate(PlanStage& stage,
                                  WorkingSet& ws,
                                  WorkingSetID& wsid,
                                  const std::vector<DoWorkResult>& expectedWorkPattern);

    void setUp() final;

    auto* expressionContext() {
        return _expCtx.get();
    }

    const auto& nss() {
        return _testNss;
    }

    /**
     * Create a simple 'index' on an empty collection with name 'indexName'.
     */
    void createIndex(BSONObj index, std::string indexName);

    /**
     * Insert the given documents into the test collection.
     */
    void insertDocs(const std::vector<BSONObj>& docs);

    /**
     * Get the index descriptor for the provided 'index'. Asserts if index isn't found.
     */
    const IndexDescriptor& getIndexDescriptor(const CollectionPtr& coll, StringData indexName);

    /**
     * A helper struct used to initialize the chunk map for the current test.
     */
    struct ChunkDesc {
        // Identifies the [min, max) bounds for the current chunk.
        ChunkRange range;
        // Specifies whether this chunk is owned by the shard that the test is running on.
        bool isOnCurShard;
    };

    /**
     * Sets up the collection metadata according to the provided map of chunks.
     */
    CollectionMetadata prepareTestData(const KeyPattern& shardKeyPattern,
                                       const std::vector<ChunkDesc>& chunkDescs);

    ~QueryShardServerTestFixture() override = default;

private:
    std::vector<ChunkType> _chunks;
    std::shared_ptr<MetadataManager> _manager;
    std::unique_ptr<ExpressionContextForTest> _expCtx;
    std::unique_ptr<DBDirectClient> _client;
    NamespaceString _testNss;
};
}  // namespace mongo
