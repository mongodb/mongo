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
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/idl/server_parameter_test_controller.h"
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

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountCreatesRecordStoreIdents) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagReplicatedFastCountDurability", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);

    // Verify idents do not exist before setup.
    ASSERT_FALSE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    setUpReplicatedFastCount(_opCtx);

    // Verify both idents exist after setup.
    ASSERT_TRUE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountIdempotentIdents) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagReplicatedFastCountDurability", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);

    ASSERT_FALSE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    setUpReplicatedFastCount(_opCtx);

    ASSERT_TRUE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);

    // Calling setup a second time should succeed and the idents should still exist.
    // TODO SERVER-122317 Verify that it isn't creating new, empty recordstores if they already
    // exist.
    _fastCountManager->shutdown(_opCtx);
    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    setUpReplicatedFastCount(_opCtx);

    ASSERT_TRUE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountSkipsContainersWhenFlagDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagReplicatedFastCountDurability", false);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);

    setUpReplicatedFastCount(_opCtx);

    // Containers should not be created when the flag is disabled.
    ASSERT_FALSE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    // Collections and manager should still be set up.
    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);
}

TEST_F(ReplicatedFastCountInitTest, StartingUpThenShuttingDownDoesNotHang) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagReplicatedFastCountDurability", true);

    const int numIterations = 100;
    for (int i = 0; i < numIterations; ++i) {
        setUpReplicatedFastCount(_opCtx);
        _fastCountManager->shutdown(_opCtx);
    }
}
}  // namespace
}  // namespace mongo
