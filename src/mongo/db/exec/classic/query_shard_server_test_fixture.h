// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/index_spec.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/shard_role/shard_catalog/metadata_manager.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

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
    const IndexCatalogEntry& getIndexEntry(const CollectionPtr& coll, std::string_view indexName);

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
