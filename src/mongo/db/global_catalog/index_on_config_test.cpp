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

#include "mongo/db/global_catalog/index_on_config.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace {

using unittest::assertGet;

using ConfigIndexTest = ConfigServerTestFixture;

TEST_F(ConfigIndexTest, CompatibleIndexAlreadyExists) {
    createIndexOnConfigCollection(operationContext(),
                                  NamespaceString::kConfigsvrShardsNamespace,
                                  BSON("host" << 1),
                                  /*unique*/ true)
        .transitional_ignore();

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto expectedShardsIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSON("v" << 2 << "unique" << true << "key" << BSON("host" << 1) << "name"
                 << "host_1")};


    auto foundShardsIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString::kConfigsvrShardsNamespace));
    assertBSONObjsSame(expectedShardsIndexes, foundShardsIndexes);
}

TEST_F(ConfigIndexTest, IncompatibleIndexAlreadyExists) {
    // Make the index non-unique even though its supposed to be unique, make sure initialization
    // fails
    createIndexOnConfigCollection(operationContext(),
                                  NamespaceString::kConfigsvrShardsNamespace,
                                  BSON("host" << 1),
                                  /*unique*/ false)
        .transitional_ignore();

    ASSERT_EQUALS(ErrorCodes::IndexKeySpecsConflict,
                  ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigIndexTest, CreateIndex) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("config.foo");

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, getIndexes(operationContext(), nss).getStatus());

    Status status =
        createIndexOnConfigCollection(operationContext(), nss, BSON("a" << 1 << "b" << 1), true);
    // Creating the index should implicitly create the collection
    ASSERT_OK(status);

    auto indexes = unittest::assertGet(getIndexes(operationContext(), nss));
    // There should be the index we just added as well as the _id index
    ASSERT_EQ(2U, indexes.size());

    // Making an identical index should be a no-op.
    status =
        createIndexOnConfigCollection(operationContext(), nss, BSON("a" << 1 << "b" << 1), true);
    ASSERT_OK(status);
    indexes = unittest::assertGet(getIndexes(operationContext(), nss));
    ASSERT_EQ(2U, indexes.size());

    // Trying to make the same index as non-unique should fail as the same index name exists
    // though unique property is part of the index signature since 4.9.
    status =
        createIndexOnConfigCollection(operationContext(), nss, BSON("a" << 1 << "b" << 1), false);
    ASSERT_EQUALS(ErrorCodes::IndexKeySpecsConflict, status);
    indexes = unittest::assertGet(getIndexes(operationContext(), nss));
    ASSERT_EQ(2U, indexes.size());
}

TEST_F(ConfigIndexTest, CreateIndexNonEmptyCollection) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("config.foo");

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, getIndexes(operationContext(), nss).getStatus());

    // Inserting the document should implicitly create the collection
    DBDirectClient dbDirectClient(operationContext());
    dbDirectClient.insert(nss, BSON("_id" << 1 << "a" << 1));

    auto status = createIndexOnConfigCollection(operationContext(), nss, BSON("a" << 1), false);
    ASSERT_OK(status);
    auto indexes = unittest::assertGet(getIndexes(operationContext(), nss));
    ASSERT_EQ(2U, indexes.size()) << BSON("indexes" << indexes);
}

}  // unnamed namespace
}  // namespace mongo
