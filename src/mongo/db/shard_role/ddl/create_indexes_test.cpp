// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class CreateIndexesTest : public CatalogTestFixture {};

TEST_F(CreateIndexesTest, CreateIndexesFailsWhenIndexBuildsCollectionIsMissing) {
    auto opCtx = operationContext();
    // Drop config.system.indexBuilds collection.
    ASSERT_OK(
        storageInterface()->dropCollection(opCtx, NamespaceString::kIndexBuildEntryNamespace));

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampMultiIndexBuilds");

    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, {}));

    {
        AutoGetCollection collection(opCtx, nss, MODE_X);
        invariant(collection);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, *collection, BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }  // Release the X lock on the collection.

    DBDirectClient client(opCtx);
    const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
    {
        auto index = BSON("v" << kIndexVersion << "key" << BSON("a" << 1) << "name"
                              << "a_1");
        auto createIndexesCmdObj = BSON(
            "createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(index) << "commitQuorum" << 1);
        BSONObj result;
        // This should fail since config.system.indexBuilds does not exist.
        unittest::LogCaptureGuard logs;
        ASSERT_FALSE(client.runCommand(nss.dbName(), createIndexesCmdObj, result)) << result;
        logs.stop();
        ASSERT_EQ(1, logs.countBSONContainingSubset(BSON("id" << 7564400)));
        ASSERT(result.hasField("code"));
        ASSERT_EQ(result.getIntField("code"), 6325700);
    }
}

TEST_F(CreateIndexesTest, CreateIndexOnPreimagesCollectionFails) {
    auto opCtx = operationContext();
    DBDirectClient client(opCtx);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("config.system.preimages");
    const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
    auto index = BSON("v" << kIndexVersion << "key" << BSON("a" << 1) << "name"
                          << "a_1");
    auto cmd = BSON("createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(index)
                                    << "commitQuorum" << 1);
    BSONObj result;
    // This should fail because it is not permitted to create indexes on config.system.preimages.
    ASSERT_FALSE(client.runCommand(nss.dbName(), cmd, result)) << result;
    ASSERT(result.hasField("code"));
    ASSERT_EQ(result.getIntField("code"), 8293400);
}

}  // namespace
}  // namespace mongo
