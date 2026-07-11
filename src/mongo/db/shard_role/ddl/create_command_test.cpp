// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

class CreateCommandTest : public CatalogTestFixture {};

TEST_F(CreateCommandTest, CreateFailsWithEncryptionOptions) {
    auto opCtx = operationContext();
    // Drop config.system.indexBuilds collection.

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("unittests.collection");

    DBDirectClient client(opCtx);

    auto cmd =
        BSON("create" << nss.coll() << "storageEngine"
                      << BSON("wiredTiger" << BSON("configString" << "encryption=(keyid=key)")));
    BSONObj result;
    // This should fail since config.system.indexBuilds does not exist.
    ASSERT_FALSE(client.runCommand(nss.dbName(), cmd, result)) << result;
    ASSERT(result.hasField("code"));
    ASSERT_EQ(result.getIntField("code"), ErrorCodes::IllegalOperation);
}

TEST_F(CreateCommandTest, LegacyCreateSucceedsWithEncryptionOptions) {
    unittest::ServerParameterGuard controller{"featureFlagBanEncryptionOptionsInCollectionCreation",
                                              false};
    auto opCtx = operationContext();
    // Drop config.system.indexBuilds collection.

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("unittests.collection");

    DBDirectClient client(opCtx);

    auto cmd =
        BSON("create" << nss.coll() << "storageEngine"
                      << BSON("wiredTiger" << BSON("configString" << "encryption=(keyid=key)")));
    BSONObj result;
    // This should fail since config.system.indexBuilds does not exist.
    ASSERT_TRUE(client.runCommand(nss.dbName(), cmd, result)) << result;
}

}  // namespace
}  // namespace mongo
