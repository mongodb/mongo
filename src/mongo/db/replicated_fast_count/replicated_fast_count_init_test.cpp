/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class ReplicatedFastCountInitTest : public CatalogTestFixture {
public:
    ReplicatedFastCountInitTest() : CatalogTestFixture() {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();
        _fastCountManager = &ReplicatedFastCountManager::get(_opCtx->getServiceContext());
    }

    void tearDown() override {
        _fastCountManager->shutdown(_opCtx);
        CatalogTestFixture::tearDown();
    }

    OperationContext* _opCtx;
    ReplicatedFastCountManager* _fastCountManager;
};

const NamespaceString replicatedFastCountStoreNss =
    NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);

const NamespaceString replicatedFastCountStoreTimestampsNss =
    NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);

TEST_F(ReplicatedFastCountInitTest,
       setUpReplicatedFastCountCreatesInternalCollectionsAndStartsUpThread) {
    {
        AutoGetCollection coll(_opCtx, replicatedFastCountStoreNss, LockMode::MODE_IS);
        ASSERT(!coll);

        AutoGetCollection collTimestamps(
            _opCtx, replicatedFastCountStoreTimestampsNss, LockMode::MODE_IS);
        ASSERT(!collTimestamps);
    }

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    setUpReplicatedFastCount(_opCtx);

    {
        AutoGetCollection coll(_opCtx, replicatedFastCountStoreNss, LockMode::MODE_IS);
        ASSERT(coll);

        AutoGetCollection collTimestamps(
            _opCtx, replicatedFastCountStoreTimestampsNss, LockMode::MODE_IS);
        ASSERT(collTimestamps);
    }

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);
}

TEST_F(ReplicatedFastCountInitTest, StartingUpThenShuttingDownDoesNotHang) {
    const int numIterations = 100;
    for (int i = 0; i < numIterations; ++i) {
        setUpReplicatedFastCount(_opCtx);
        _fastCountManager->shutdown(_opCtx);
    }
}
}  // namespace
}  // namespace mongo
