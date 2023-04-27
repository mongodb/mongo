/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/concurrency/resource_catalog.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class ResourceCatalogTest : public unittest::Test {
public:
    void setUp() {
        ASSERT_EQ(firstResourceId, secondResourceId);
        ASSERT_NE(firstResourceId, thirdResourceId);
    }

protected:
    NamespaceString firstCollection =
        NamespaceString::createNamespaceString_forTest(boost::none, "1661880728");
    ResourceId firstResourceId{RESOURCE_COLLECTION, firstCollection};

    NamespaceString secondCollection =
        NamespaceString::createNamespaceString_forTest(boost::none, "1626936312");
    ResourceId secondResourceId{RESOURCE_COLLECTION, secondCollection};

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
    ASSERT_EQ(firstCollection.toStringWithTenantId(), *resource);

    resource = catalog.name(thirdResourceId);
    ASSERT_EQ(thirdCollection.toStringWithTenantId(), resource);
}

TEST_F(ResourceCatalogTest, RemoveTest) {
    catalog.add(firstResourceId, firstCollection);
    catalog.add(thirdResourceId, thirdCollection);

    // This fails to remove the resource because of an invalid namespace.
    catalog.remove(firstResourceId,
                   NamespaceString::createNamespaceString_forTest(boost::none, "BadNamespace"));
    auto resource = catalog.name(firstResourceId);
    ASSERT_EQ(firstCollection.toStringWithTenantId(), *resource);

    catalog.remove(firstResourceId, firstCollection);
    catalog.remove(firstResourceId, firstCollection);
    catalog.remove(thirdResourceId, thirdCollection);

    resource = catalog.name(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    resource = catalog.name(thirdResourceId);
    ASSERT_EQ(boost::none, resource);
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
    ASSERT_EQ(secondCollection.toStringWithTenantId(), *resource);

    // Adding the same namespace twice does not create a collision.
    catalog.add(secondResourceId, secondCollection);
    resource = catalog.name(secondResourceId);
    ASSERT_EQ(secondCollection.toStringWithTenantId(), *resource);

    // The map should function normally for entries without collisions.
    catalog.add(firstResourceId, firstCollection);
    resource = catalog.name(secondResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.add(thirdResourceId, thirdCollection);
    resource = catalog.name(thirdResourceId);
    ASSERT_EQ(thirdCollection.toStringWithTenantId(), *resource);

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

DEATH_TEST_F(ResourceCatalogTest, AddDatabaseInvalidResourceType, "invariant") {
    catalog.add({RESOURCE_GLOBAL, 0}, DatabaseName::createDatabaseName_forTest(boost::none, "db"));
}

DEATH_TEST_F(ResourceCatalogTest, AddCollectionInvalidResourceType, "invariant") {
    catalog.add({RESOURCE_GLOBAL, 0}, NamespaceString::createNamespaceString_forTest("db.coll"));
}
}  // namespace
}  // namespace mongo
