// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

constexpr int kIndexVersion = 2;

class ShardKeyIndexUtilTest : public CatalogTestFixture {
public:
    ShardKeyIndexUtilTest() : CatalogTestFixture() {}

    void createIndex(const BSONObj& spec) {
        // Build the specified index on the collection.
        WriteUnitOfWork wuow(opCtx());
        CollectionWriter writer{opCtx(), *_coll};

        auto* indexCatalog = writer.getWritableCollection(opCtx())->getIndexCatalog();
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(
            opCtx(), writer.getWritableCollection(opCtx()), spec));
        wuow.commit();
    }

    const NamespaceString& nss() const {
        return _nss;
    }

    const CollectionPtr& coll() const {
        return *_coll.get();
    }

    OperationContext* opCtx() {
        return operationContext();
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _coll.emplace(opCtx(), _nss, MODE_X);
        ASSERT_OK(storageInterface()->createCollection(opCtx(), _nss, {}));
    }

    void tearDown() override {
        _coll.reset();
        CatalogTestFixture::tearDown();
    }

private:
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.user");
    boost::optional<AutoGetCollection> _coll;
};

TEST_F(ShardKeyIndexUtilTest, SimpleKeyPattern) {
    createIndex(BSON("key" << BSON("y" << 1) << "name"
                           << "y"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));

    const auto index =
        findShardKeyPrefixedIndex(opCtx(), coll(), BSON("x" << 1), true /* requireSingleKey */);

    ASSERT_TRUE(index);
    ASSERT_EQ("x", index->indexEntry()->descriptor()->indexName());
}

TEST_F(ShardKeyIndexUtilTest, HashedKeyPattern) {
    createIndex(BSON("key" << BSON("y" << 1) << "name"
                           << "y"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << "hashed") << "name"
                           << "xhashed"
                           << "v" << kIndexVersion));

    const auto index = findShardKeyPrefixedIndex(
        opCtx(), coll(), BSON("x" << "hashed"), true /* requireSingleKey */);

    ASSERT_TRUE(index);
    ASSERT_EQ("xhashed", index->indexEntry()->descriptor()->indexName());
}

TEST_F(ShardKeyIndexUtilTest, PrefixKeyPattern) {
    createIndex(BSON("key" << BSON("y" << 1) << "name"
                           << "y"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1 << "y" << 1 << "z" << 1) << "name"
                           << "xyz"
                           << "v" << kIndexVersion));

    const auto index = findShardKeyPrefixedIndex(
        opCtx(), coll(), BSON("x" << 1 << "y" << 1), true /* requireSingleKey */);

    ASSERT_TRUE(index);
    ASSERT_EQ("xyz", index->indexEntry()->descriptor()->indexName());
}

TEST_F(ShardKeyIndexUtilTest, ExcludesIncompatibleIndexes) {
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "sparse"
                           << "sparse" << true << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "partial"
                           << "partialFilterExpression" << BSON("x" << BSON("$exists" << true))
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "collation"
                           << "collation" << BSON("locale" << "fr") << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1 << "a.$**" << 1) << "name"
                           << "wildcard" << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("y" << 1) << "name" << "y" << "v" << kIndexVersion));
    createIndex(
        BSON("key" << BSON("y" << 1 << "x" << 1) << "name" << "y_x" << "v" << kIndexVersion));

    {
        const auto index =
            findShardKeyPrefixedIndex(opCtx(), coll(), BSON("x" << 1), true /* requireSingleKey */);

        ASSERT_FALSE(index);
    }

    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));

    {
        const auto index =
            findShardKeyPrefixedIndex(opCtx(), coll(), BSON("x" << 1), true /* requireSingleKey */);

        ASSERT_TRUE(index);
        ASSERT_EQ("x", index->indexEntry()->descriptor()->indexName());
    }
}

TEST_F(ShardKeyIndexUtilTest, MultiKeyIndexIsAnIncompatibleShardKey) {
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));

    DBDirectClient client(opCtx());
    client.insert(nss(), BSON("x" << BSON_ARRAY(1 << 2)));

    const auto index =
        findShardKeyPrefixedIndex(opCtx(), coll(), BSON("x" << 1), true /* requireSingleKey */);

    ASSERT_FALSE(index);
}

TEST_F(ShardKeyIndexUtilTest, ExcludesMultiKeyIfRequiresSingleKey) {
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));

    DBDirectClient client(opCtx());
    client.insert(nss(), BSON("x" << BSON_ARRAY(1 << 2)));

    const auto index =
        findShardKeyPrefixedIndex(opCtx(), coll(), BSON("x" << 1), true /* requireSingleKey */);

    ASSERT_FALSE(index);
}

TEST_F(ShardKeyIndexUtilTest, IncludesMultiKeyIfSingleKeyNotRequired) {
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));

    DBDirectClient client(opCtx());
    client.insert(nss(), BSON("x" << BSON_ARRAY(1 << 2)));

    const auto index =
        findShardKeyPrefixedIndex(opCtx(), coll(), BSON("x" << 1), false /* requireSingleKey */);

    ASSERT_TRUE(index);
    ASSERT_EQ("x", index->indexEntry()->descriptor()->indexName());
}

TEST_F(ShardKeyIndexUtilTest, LastShardIndexWithSingleCandidate) {
    createIndex(BSON("key" << BSON("y" << 1) << "name"
                           << "y"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1 << "y" << 1) << "name"
                           << "xy"
                           << "v" << kIndexVersion << "hidden" << true));

    ASSERT_TRUE(isLastNonHiddenRangedShardKeyIndex(opCtx(), coll(), "x", BSON("x" << 1)));
}

TEST_F(ShardKeyIndexUtilTest, LastShardIndexWithMultipleCandidates) {
    createIndex(BSON("key" << BSON("y" << 1) << "name"
                           << "y"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1 << "y" << 1) << "name"
                           << "xy"
                           << "v" << kIndexVersion));

    ASSERT_FALSE(isLastNonHiddenRangedShardKeyIndex(opCtx(), coll(), "x", BSON("x" << 1)));
}

TEST_F(ShardKeyIndexUtilTest, LastShardIndexWithIncompatibleIndex) {
    createIndex(BSON("key" << BSON("y" << 1) << "name"
                           << "y"
                           << "v" << kIndexVersion));
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));

    ASSERT_FALSE(isLastNonHiddenRangedShardKeyIndex(opCtx(), coll(), "y", BSON("x" << 1)));
}

TEST_F(ShardKeyIndexUtilTest, LastShardIndexWithNonExistingIndex) {
    createIndex(BSON("key" << BSON("x" << 1) << "name"
                           << "x"
                           << "v" << kIndexVersion));

    ASSERT_FALSE(isLastNonHiddenRangedShardKeyIndex(opCtx(), coll(), "y", BSON("x" << 1)));
}

TEST_F(ShardKeyIndexUtilTest, LastShardIndexExcludesHashedIndex) {
    createIndex(BSON("key" << BSON("x" << "hashed") << "name"
                           << "xhashed"
                           << "v" << kIndexVersion));

    ASSERT_FALSE(isLastNonHiddenRangedShardKeyIndex(opCtx(), coll(), "y", BSON("x" << "hashed")));
}

TEST_F(ShardKeyIndexUtilTest, LastShardIndexExcludesCompoundHashedIndex) {
    createIndex(BSON("key" << BSON("x" << "hashed"
                                       << "y" << 1)
                           << "name"
                           << "xhashed"
                           << "v" << kIndexVersion));

    ASSERT_FALSE(isLastNonHiddenRangedShardKeyIndex(opCtx(), coll(), "y", BSON("x" << "hashed")));
}

}  // namespace
}  // namespace mongo
