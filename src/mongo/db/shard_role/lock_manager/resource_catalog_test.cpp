// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/resource_catalog.h"

#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class ResourceCatalogTest : public unittest::Test {
public:
    void setUp() override {
        ASSERT_EQ(firstResourceId, secondResourceId);
        ASSERT_NE(firstResourceId, thirdResourceId);
    }

protected:
    NamespaceString firstCollection =
        NamespaceString::createNamespaceString_forTest(boost::none, "1661880728");
    ResourceId firstResourceId{RESOURCE_COLLECTION, firstCollection};

    NamespaceString secondCollection =
        NamespaceString::createNamespaceString_forTest(boost::none, "1626936312");
    // Use a duplicate ResourceId to simulate ResourceId hash collisions.
    ResourceId secondResourceId = firstResourceId;

    NamespaceString thirdCollection =
        NamespaceString::createNamespaceString_forTest(boost::none, "2930102946");
    ResourceId thirdResourceId{RESOURCE_COLLECTION, thirdCollection};

    ResourceCatalog catalog;
};

TEST_F(ResourceCatalogTest, EmptyTest) {
    auto resource = catalog.name(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.remove(secondResourceId, secondCollection);
    resource = catalog.name(secondResourceId);
    ASSERT_EQ(boost::none, resource);
}

TEST_F(ResourceCatalogTest, InsertTest) {
    catalog.add(firstResourceId, firstCollection);
    auto resource = catalog.name(thirdResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.add(thirdResourceId, thirdCollection);

    resource = catalog.name(firstResourceId);
    ASSERT_EQ(firstCollection.toStringWithTenantId_forTest(), *resource);

    resource = catalog.name(thirdResourceId);
    ASSERT_EQ(thirdCollection.toStringWithTenantId_forTest(), resource);
}

TEST_F(ResourceCatalogTest, RemoveTest) {
    catalog.add(firstResourceId, firstCollection);
    catalog.add(thirdResourceId, thirdCollection);

    // This fails to remove the resource because of an invalid namespace.
    catalog.remove(firstResourceId,
                   NamespaceString::createNamespaceString_forTest(boost::none, "BadNamespace"));
    auto resource = catalog.name(firstResourceId);
    ASSERT_EQ(firstCollection.toStringWithTenantId_forTest(), *resource);

    catalog.remove(firstResourceId, firstCollection);
    catalog.remove(firstResourceId, firstCollection);
    catalog.remove(thirdResourceId, thirdCollection);

    resource = catalog.name(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    resource = catalog.name(thirdResourceId);
    ASSERT_EQ(boost::none, resource);
}

TEST_F(ResourceCatalogTest, ResourceMutexTest) {
    auto rid = catalog.newResourceIdForMutex("TestLabel");
    ASSERT_EQ("TestLabel", *catalog.name(rid));
}

TEST_F(ResourceCatalogTest, CollisionTest) {
    // firstCollection and secondCollection map to the same ResourceId.
    catalog.add(firstResourceId, firstCollection);
    catalog.add(secondResourceId, secondCollection);

    // Looking up the namespace on a ResourceId while it has a collision should
    // return the empty string.
    auto resource = catalog.name(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    resource = catalog.name(secondResourceId);
    ASSERT_EQ(boost::none, resource);

    // We remove a namespace, resolving the collision.
    catalog.remove(firstResourceId, firstCollection);
    resource = catalog.name(secondResourceId);
    ASSERT_EQ(secondCollection.toStringWithTenantId_forTest(), *resource);

    // Adding the same namespace twice does not create a collision.
    catalog.add(secondResourceId, secondCollection);
    resource = catalog.name(secondResourceId);
    ASSERT_EQ(secondCollection.toStringWithTenantId_forTest(), *resource);

    // The map should function normally for entries without collisions.
    catalog.add(firstResourceId, firstCollection);
    resource = catalog.name(secondResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.add(thirdResourceId, thirdCollection);
    resource = catalog.name(thirdResourceId);
    ASSERT_EQ(thirdCollection.toStringWithTenantId_forTest(), *resource);

    catalog.remove(thirdResourceId, thirdCollection);
    resource = catalog.name(thirdResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.remove(firstResourceId, firstCollection);
    catalog.remove(secondResourceId, secondCollection);

    resource = catalog.name(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    resource = catalog.name(secondResourceId);
    ASSERT_EQ(boost::none, resource);
}

using ResourceCatalogTestDeathTest = ResourceCatalogTest;
DEATH_TEST_F(ResourceCatalogTestDeathTest, AddDatabaseInvalidResourceType, "invariant") {
    catalog.add({RESOURCE_GLOBAL, 0}, DatabaseName::createDatabaseName_forTest(boost::none, "db"));
}

DEATH_TEST_F(ResourceCatalogTestDeathTest, AddCollectionInvalidResourceType, "invariant") {
    catalog.add({RESOURCE_GLOBAL, 0}, NamespaceString::createNamespaceString_forTest("db.coll"));
}

}  // namespace
}  // namespace mongo
