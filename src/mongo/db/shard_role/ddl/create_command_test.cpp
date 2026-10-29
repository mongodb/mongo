// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

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

TEST_F(CreateCommandTest, CreateLocalCollectionFailsWhenUnsupported) {
    // Register a custom persistence provider which disallows local collections
    struct NoLocalCollectionsPersistenceProvider : public rss::AttachedPersistenceProvider {
        bool supportsLocalCollections() const override {
            return false;
        }
    };
    rss::ReplicatedStorageService::get(getServiceContext())
        .setPersistenceProvider(std::make_unique<NoLocalCollectionsPersistenceProvider>());

    auto opCtx = operationContext();
    DBDirectClient client(opCtx);

    BSONObj result;
    {
        auto nss = NamespaceString::createNamespaceString_forTest("local.collection");
        auto cmd = BSON("create" << nss.coll());
        ASSERT_FALSE(client.runCommand(nss.dbName(), cmd, result)) << result;
        ASSERT_EQ(result.getIntField("code"), ErrorCodes::InvalidNamespace);
    }

    {
        auto nss = NamespaceString::createNamespaceString_forTest("db.system.profile");
        auto cmd = BSON("create" << nss.coll());
        ASSERT_FALSE(client.runCommand(nss.dbName(), cmd, result)) << result;
        ASSERT_EQ(result.getIntField("code"), ErrorCodes::InvalidNamespace);
    }
}

}  // namespace
}  // namespace mongo
