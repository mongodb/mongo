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

#include "mongo/db/s/move_primary/move_primary_cloner_test_fixture.h"

#include "mongo/db/clientcursor.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/move_primary/move_primary_collection_cloner.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class MovePrimaryCollectionClonerTest : public MovePrimaryClonerTestFixture {
public:
    MovePrimaryCollectionClonerTest() {}

protected:
    void setUp() override {
        MovePrimaryClonerTestFixture::setUp();
    }

    void tearDown() {
        MovePrimaryClonerTestFixture::tearDown();
    }

    std::unique_ptr<MovePrimaryCollectionCloner> makeCollectionCloner(
        MovePrimarySharedData* sharedData = nullptr) {
        MovePrimaryBaseCloner::CollectionParams collectionParams{_nss, UUID::gen()};
        return std::make_unique<MovePrimaryCollectionCloner>(collectionParams,
                                                             sharedData ? sharedData
                                                                        : getSharedData(),
                                                             _source,
                                                             _mockClient.get(),
                                                             &_storageInterface,
                                                             _dbWorkThreadPool.get());
    }

    const std::string _dbName = "_testDb";
    const NamespaceString _nss = {"_testDb", "testcoll"};
    CollectionOptions _options;
};

TEST_F(MovePrimaryCollectionClonerTest, DummyTest) {
    MovePrimarySharedData dummySharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeCollectionCloner(&dummySharedData);
}

}  // namespace mongo
