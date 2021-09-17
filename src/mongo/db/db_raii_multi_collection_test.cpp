/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/db_raii.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class AutoGetCollectionMultiTest : public CatalogTestFixture {
public:
    AutoGetCollectionMultiTest() : CatalogTestFixture("wiredTiger") {}

    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientWithLocker(const std::string& clientName) {
        auto client = getServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        client->swapLockState(std::make_unique<LockerImpl>());
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    void createCollections(OperationContext* opCtx) {
        CollectionOptions defaultCollectionOptions;
        ASSERT_OK(
            storageInterface()->createCollection(opCtx, _primaryNss, defaultCollectionOptions));
        ASSERT_OK(
            storageInterface()->createCollection(opCtx, _secondaryNss1, defaultCollectionOptions));
        ASSERT_OK(
            storageInterface()->createCollection(opCtx, _secondaryNss2, defaultCollectionOptions));
        ASSERT_OK(
            storageInterface()->createCollection(opCtx, _otherDBNss, defaultCollectionOptions));
    }

    const NamespaceString _primaryNss = NamespaceString("primary1.db1");
    const NamespaceString _secondaryNss1 = NamespaceString("secondary1.db1");
    const NamespaceString _secondaryNss2 = NamespaceString("secondary2.db1");
    const NamespaceString _otherDBNss = NamespaceString("secondary2.db2");

    std::vector<NamespaceStringOrUUID> _secondaryNssVec = {NamespaceStringOrUUID(_secondaryNss1),
                                                           NamespaceStringOrUUID(_secondaryNss2)};
    std::vector<NamespaceStringOrUUID> _otherDBNssVec = {NamespaceStringOrUUID(_otherDBNss)};

    const ClientAndCtx _client1 = makeClientWithLocker("client1");
    const ClientAndCtx _client2 = makeClientWithLocker("client2");
};

TEST_F(AutoGetCollectionMultiTest, SingleDB) {
    auto opCtx1 = _client1.second.get();

    createCollections(opCtx1);

    AutoGetCollectionMultiForReadCommandLockFree autoGet(
        opCtx1, NamespaceStringOrUUID(_primaryNss), _secondaryNssVec);

    auto locker = opCtx1->lockState();
    locker->dump();
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(!locker->isDbLockedForMode(_primaryNss.db(), MODE_IS));
    // Set 'shouldConflictWithSecondaryBatchApplication' to true so isCollectionLockedForMode()
    // doesn't return true regardless of what locks are held.
    opCtx1->lockState()->setShouldConflictWithSecondaryBatchApplication(true);
    invariant(!locker->isCollectionLockedForMode(_primaryNss, MODE_IS));

    const auto& coll = autoGet.getCollection();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss1));
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss2));

    coll.yield();
    coll.restore();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss1));
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss2));
}

TEST_F(AutoGetCollectionMultiTest, MultiDBs) {
    auto opCtx1 = _client1.second.get();

    createCollections(opCtx1);

    AutoGetCollectionMultiForReadCommandLockFree autoGet(
        opCtx1, NamespaceStringOrUUID(_primaryNss), _otherDBNssVec);

    auto locker = opCtx1->lockState();
    locker->dump();
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(!locker->isDbLockedForMode(_primaryNss.db(), MODE_IS));
    invariant(!locker->isDbLockedForMode(_otherDBNss.db(), MODE_IS));
    // Set 'shouldConflictWithSecondaryBatchApplication' to true so isCollectionLockedForMode()
    // doesn't return true regardless of what locks are held.
    opCtx1->lockState()->setShouldConflictWithSecondaryBatchApplication(true);
    invariant(!locker->isCollectionLockedForMode(_primaryNss, MODE_IS));

    const auto& coll = autoGet.getCollection();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _otherDBNss));

    coll.yield();
    coll.restore();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _otherDBNss));
}

TEST_F(AutoGetCollectionMultiTest, DropCollection) {
    auto opCtx1 = _client1.second.get();

    createCollections(opCtx1);

    AutoGetCollectionMultiForReadCommandLockFree autoGet(
        opCtx1, NamespaceStringOrUUID(_primaryNss), _secondaryNssVec);

    auto locker = opCtx1->lockState();
    locker->dump();
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(!locker->isDbLockedForMode(_primaryNss.db(), MODE_IS));
    // Set 'shouldConflictWithSecondaryBatchApplication' to true so isCollectionLockedForMode()
    // doesn't return true regardless of what locks are held.
    opCtx1->lockState()->setShouldConflictWithSecondaryBatchApplication(true);
    invariant(!locker->isCollectionLockedForMode(_primaryNss, MODE_IS));

    const auto& coll = autoGet.getCollection();

    // Drop a secondary collection via a different opCtx, so that we can test that yield restore
    // throws on failing to verify the secondary collection.
    {
        auto opCtx2 = _client2.second.get();
        AutoGetCollection secondClientAutoGet(opCtx2, _secondaryNss1, MODE_X);

        // Disable replication so that it is not necessary to set up the infrastructure to timestamp
        // catalog writes properly.
        repl::UnreplicatedWritesBlock uwb(opCtx2);

        ASSERT_OK(storageInterface()->dropCollection(opCtx2, _secondaryNss1));
    }

    coll.yield();
    ASSERT_THROWS_CODE(coll.restore(), AssertionException, ErrorCodes::NamespaceNotFound);
}

TEST_F(AutoGetCollectionMultiTest, DropAndRecreateCollection) {
    auto opCtx1 = _client1.second.get();

    createCollections(opCtx1);

    AutoGetCollectionMultiForReadCommandLockFree autoGet(
        opCtx1, NamespaceStringOrUUID(_primaryNss), _secondaryNssVec);

    auto locker = opCtx1->lockState();
    locker->dump();
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(!locker->isDbLockedForMode(_primaryNss.db(), MODE_IS));
    // Set 'shouldConflictWithSecondaryBatchApplication' to true so isCollectionLockedForMode()
    // doesn't return true regardless of what locks are held.
    opCtx1->lockState()->setShouldConflictWithSecondaryBatchApplication(true);
    invariant(!locker->isCollectionLockedForMode(_primaryNss, MODE_IS));

    const auto& coll = autoGet.getCollection();

    // Drop and recreate a secondary collection via a different opCtx, so that we can test that
    // yield restore throws on failing to verify the secondary collection.
    {
        auto opCtx2 = _client2.second.get();
        AutoGetCollection secondClientAutoGet(opCtx2, _secondaryNss1, MODE_X);

        // Disable replication so that it is not necessary to set up the infrastructure to timestamp
        // catalog writes properly.
        repl::UnreplicatedWritesBlock uwb(opCtx2);

        ASSERT_OK(storageInterface()->dropCollection(_client2.second.get(), _secondaryNss1));
        CollectionOptions defaultCollectionOptions;
        ASSERT_OK(
            storageInterface()->createCollection(opCtx2, _secondaryNss1, defaultCollectionOptions));
    }

    coll.yield();
    ASSERT_THROWS_CODE(coll.restore(), AssertionException, ErrorCodes::NamespaceNotFound);
}

}  // namespace
}  // namespace mongo
