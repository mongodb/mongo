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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/locker_impl.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class AutoGetCollectionMultiTest : public CatalogTestFixture {
public:
    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientWithLocker(const std::string& clientName) {
        auto client = getServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        client->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
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
        ASSERT_OK(storageInterface()->createCollection(
            opCtx, _secondaryNssOtherDbNss, defaultCollectionOptions));
    }

    void createCollectionsExceptOneSecondary(OperationContext* opCtx) {
        CollectionOptions defaultCollectionOptions;
        ASSERT_OK(
            storageInterface()->createCollection(opCtx, _primaryNss, defaultCollectionOptions));
        ASSERT_OK(
            storageInterface()->createCollection(opCtx, _secondaryNss1, defaultCollectionOptions));
        ASSERT_OK(storageInterface()->createCollection(
            opCtx, _secondaryNssOtherDbNss, defaultCollectionOptions));
    }

    const NamespaceString _primaryNss =
        NamespaceString::createNamespaceString_forTest("db1.primary1");
    const NamespaceString _secondaryNss1 =
        NamespaceString::createNamespaceString_forTest("db1.secondary1");
    const NamespaceString _secondaryNss2 =
        NamespaceString::createNamespaceString_forTest("db1.secondary2");

    const std::vector<NamespaceStringOrUUID> _secondaryNssOrUUIDVec = {
        NamespaceStringOrUUID(_secondaryNss1), NamespaceStringOrUUID(_secondaryNss2)};

    const NamespaceString _secondaryNssOtherDbNss =
        NamespaceString::createNamespaceString_forTest("db2.secondary1");
    const std::vector<NamespaceStringOrUUID> _secondaryNssOtherDbNssVec = {
        NamespaceStringOrUUID(_secondaryNssOtherDbNss)};

    const std::vector<NamespaceStringOrUUID> _secondaryNssOrUUIDAllVec = {
        NamespaceStringOrUUID(_secondaryNss1),
        NamespaceStringOrUUID(_secondaryNss2),
        NamespaceStringOrUUID(_secondaryNssOtherDbNss)};

    std::vector<NamespaceString> _secondaryNamespacesAll{
        _secondaryNss1, _secondaryNss2, _secondaryNssOtherDbNss};

    const ClientAndCtx _client1 = makeClientWithLocker("client1");
    const ClientAndCtx _client2 = makeClientWithLocker("client2");
};

TEST_F(AutoGetCollectionMultiTest, SecondaryNssMinimumVisible) {
    auto opCtx1 = _client1.second.get();

    // Create a primary and two secondary collections to lock. _secondaryNss1 will not be used later
    // for locking. Instead, the timestamp of _secondaryNss1's creation will be used as the read
    // timestamp to ensure that an AutoGetCollectionForRead can still be instantiated but where the
    // the collection _secondaryNss2 will be non-existent.
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(storageInterface()->createCollection(opCtx1, _primaryNss, defaultCollectionOptions));
    ASSERT_OK(
        storageInterface()->createCollection(opCtx1, _secondaryNss1, defaultCollectionOptions));
    ASSERT_OK(
        storageInterface()->createCollection(opCtx1, _secondaryNss2, defaultCollectionOptions));

    // Set the read source earlier than Collection _secondaryNss2' min valid timestamp, but later
    // than _primaryNss' min visible timestamp.
    opCtx1->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, [&]() {
        AutoGetCollection secondaryCollection1(opCtx1, _secondaryNss1, MODE_IS);
        return secondaryCollection1->getMinimumValidSnapshot();
    }());

    // Create the AutoGet* instance on multiple collections.
    std::vector<NamespaceStringOrUUID> secondaryNamespaces{NamespaceStringOrUUID(_secondaryNss1),
                                                           NamespaceStringOrUUID(_secondaryNss2)};
    AutoGetCollectionForRead autogetForRead(
        opCtx1,
        _primaryNss,
        AutoGetCollection::Options{}.secondaryNssOrUUIDs(secondaryNamespaces.cbegin(),
                                                         secondaryNamespaces.cend()));

    // We can see the collections at `_primaryNss` and `_secondaryNss1` but not `_secondaryNss2`.
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _primaryNss));
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss1));
    ASSERT(!CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss2));
}

TEST_F(AutoGetCollectionMultiTest, LockFreeMultiCollectionSingleDB) {
    auto opCtx1 = _client1.second.get();

    createCollections(opCtx1);

    invariant(!opCtx1->lockState()->isCollectionLockedForMode(_primaryNss, MODE_IS));

    AutoGetCollectionForReadLockFree autoGet(
        opCtx1,
        NamespaceStringOrUUID(_primaryNss),
        AutoGetCollection::Options{}.secondaryNssOrUUIDs(_secondaryNssOrUUIDVec.cbegin(),
                                                         _secondaryNssOrUUIDVec.cend()));

    auto locker = opCtx1->lockState();
    locker->dump();
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(!locker->isDbLockedForMode(_primaryNss.dbName(), MODE_IS));
    invariant(!locker->isCollectionLockedForMode(_primaryNss, MODE_IS));

    const auto& coll = autoGet.getCollection();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss1));
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss2));
}

TEST_F(AutoGetCollectionMultiTest, LockedDuplicateNamespaces) {
    auto opCtx1 = _client1.second.get();

    const std::vector<NamespaceStringOrUUID> duplicateNssVector = {
        NamespaceStringOrUUID(_primaryNss),
        NamespaceStringOrUUID(_primaryNss),
        NamespaceStringOrUUID(_secondaryNss1),
        NamespaceStringOrUUID(_secondaryNss1)};

    createCollections(opCtx1);

    invariant(!opCtx1->lockState()->isCollectionLockedForMode(_primaryNss, MODE_IS));

    AutoGetCollectionForRead autoGet(opCtx1,
                                     NamespaceStringOrUUID(_primaryNss),
                                     AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                         duplicateNssVector.cbegin(), duplicateNssVector.cend()));

    auto locker = opCtx1->lockState();
    locker->dump();
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(locker->isDbLockedForMode(_primaryNss.dbName(), MODE_IS));
    invariant(locker->isDbLockedForMode(_secondaryNss1.dbName(), MODE_IS));
    invariant(locker->isCollectionLockedForMode(_primaryNss, MODE_IS));
    invariant(locker->isCollectionLockedForMode(_secondaryNss1, MODE_IS));

    const auto& coll = autoGet.getCollection();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss1));
}

TEST_F(AutoGetCollectionMultiTest, LockFreeMultiDBs) {
    auto opCtx1 = _client1.second.get();

    createCollections(opCtx1);

    AutoGetCollectionForReadLockFree autoGet(
        opCtx1,
        NamespaceStringOrUUID(_primaryNss),
        AutoGetCollection::Options{}.secondaryNssOrUUIDs(_secondaryNssOtherDbNssVec.cbegin(),
                                                         _secondaryNssOtherDbNssVec.cend()));

    auto locker = opCtx1->lockState();
    locker->dump();
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(!locker->isDbLockedForMode(_primaryNss.dbName(), MODE_IS));
    invariant(!locker->isDbLockedForMode(_secondaryNssOtherDbNss.dbName(), MODE_IS));
    invariant(!locker->isCollectionLockedForMode(_primaryNss, MODE_IS));

    const auto& coll = autoGet.getCollection();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1,
                                                                       _secondaryNssOtherDbNss));
}

TEST_F(AutoGetCollectionMultiTest, LockFreeSecondaryNamespaceNotFoundIsOK) {
    auto opCtx1 = _client1.second.get();

    createCollectionsExceptOneSecondary(opCtx1);

    AutoGetCollectionForReadLockFree autoGet(
        opCtx1,
        NamespaceStringOrUUID(_primaryNss),
        AutoGetCollection::Options{}.secondaryNssOrUUIDs(_secondaryNssOrUUIDAllVec.cbegin(),
                                                         _secondaryNssOrUUIDAllVec.cend()));

    invariant(opCtx1->lockState()->isLocked());
    ASSERT(!CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss2));
}

TEST_F(AutoGetCollectionMultiTest, LockedSecondaryNamespaceNotFound) {
    auto opCtx1 = _client1.second.get();

    createCollectionsExceptOneSecondary(opCtx1);

    AutoGetCollectionForRead autoGet(
        opCtx1,
        NamespaceStringOrUUID(_primaryNss),
        AutoGetCollection::Options{}.secondaryNssOrUUIDs(_secondaryNssOrUUIDVec.cbegin(),
                                                         _secondaryNssOrUUIDVec.cend()));

    auto locker = opCtx1->lockState();

    invariant(locker->isLocked());
    invariant(locker->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    invariant(locker->isDbLockedForMode(_primaryNss.dbName(), MODE_IS));
    invariant(locker->isCollectionLockedForMode(_primaryNss, MODE_IS));

    for (const auto& secondaryNss : _secondaryNssOrUUIDVec) {
        invariant(locker->isDbLockedForMode(secondaryNss.nss().dbName(), MODE_IS));
        invariant(locker->isCollectionLockedForMode(secondaryNss.nss(), MODE_IS));
    }

    const auto& coll = autoGet.getCollection();
    ASSERT(coll);
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss1));
    ASSERT(CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1,
                                                                       _secondaryNssOtherDbNss));
    ASSERT(!CollectionCatalog::get(opCtx1)->lookupCollectionByNamespace(opCtx1, _secondaryNss2));
}

}  // namespace
}  // namespace mongo
