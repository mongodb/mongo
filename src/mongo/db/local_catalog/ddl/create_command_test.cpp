/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/server_parameter_test_controller.h"
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
    RAIIServerParameterControllerForTest controller{
        "featureFlagBanEncryptionOptionsInCollectionCreation", false};
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
