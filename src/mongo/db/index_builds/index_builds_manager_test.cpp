/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/index_builds/index_builds_manager.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <string>

namespace mongo {
namespace {

class IndexBuildsManagerTest : public CatalogTestFixture {
private:
    void setUp() override;
    void tearDown() override;

public:
    void createCollection(const NamespaceString& nss);

    std::vector<IndexBuildInfo> makeSpecs(std::vector<std::string> keys);

    const UUID _buildUUID = UUID::gen();
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.foo");
    IndexBuildsManager _indexBuildsManager;
};

void IndexBuildsManagerTest::setUp() {
    CatalogTestFixture::setUp();
    createCollection(_nss);
}

void IndexBuildsManagerTest::tearDown() {
    _indexBuildsManager.verifyNoIndexBuilds_forTestOnly();
    // All databases are dropped during tear down.
    CatalogTestFixture::tearDown();
}

void IndexBuildsManagerTest::createCollection(const NamespaceString& nss) {
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
}

std::vector<IndexBuildInfo> IndexBuildsManagerTest::makeSpecs(std::vector<std::string> keys) {
    ASSERT(keys.size());
    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    std::vector<IndexBuildInfo> indexes;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& keyName = keys[i];
        IndexBuildInfo indexBuildInfo(
            BSON("v" << 2 << "key" << BSON(keyName << 1) << "name" << (keyName + "_1")),
            fmt::format("index-{}", i + 1));
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        indexes.push_back(std::move(indexBuildInfo));
    }
    return indexes;
}

TEST_F(IndexBuildsManagerTest, IndexBuildsManagerSetUpAndTearDown) {
    AutoGetCollection autoColl(operationContext(), _nss, MODE_X);
    CollectionWriter collection(operationContext(), autoColl);

    auto indexes = makeSpecs({"a", "b"});
    ASSERT_OK(_indexBuildsManager.setUpIndexBuild(
        operationContext(), collection, indexes, _buildUUID, MultiIndexBlock::kNoopOnInitFn));

    _indexBuildsManager.abortIndexBuild(
        operationContext(), collection, _buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
    _indexBuildsManager.tearDownAndUnregisterIndexBuild(_buildUUID);
}
}  // namespace

}  // namespace mongo
