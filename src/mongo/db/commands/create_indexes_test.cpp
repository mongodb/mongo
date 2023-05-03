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

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"

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
        startCapturingLogMessages();
        ASSERT_FALSE(client.runCommand(nss.dbName(), createIndexesCmdObj, result)) << result;
        stopCapturingLogMessages();
        ASSERT_EQ(1, countBSONFormatLogLinesIsSubset(BSON("id" << 7564400)));
        ASSERT(result.hasField("code"));
        ASSERT_EQ(result.getIntField("code"), 6325700);
    }
}

}  // namespace
}  // namespace mongo
