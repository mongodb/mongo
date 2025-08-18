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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>

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
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *collection, InsertStatement(BSONObj(BSON("_id" << 0 << "a" << 1))), nullptr));
        wuow.commit();
    }  // Release the X lock on the collection.

    DBDirectClient client(opCtx);
    const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
    {
        // Disable index build commit quorum as we don't have support of replication subsystem
        // for voting.
        auto index = BSON("v" << kIndexVersion << "key" << BSON("a" << 1) << "name"
                              << "a_1");
        auto createIndexesCmdObj = BSON(
            "createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(index) << "commitQuorum" << 0);
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
                                    << "commitQuorum" << 0);
    BSONObj result;
    // This should fail because it is not permitted to create indexes on config.system.preimages.
    ASSERT_FALSE(client.runCommand(nss.dbName(), cmd, result)) << result;
    ASSERT(result.hasField("code"));
    ASSERT_EQ(result.getIntField("code"), 8293400);
}

}  // namespace
}  // namespace mongo
