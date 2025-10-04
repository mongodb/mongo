/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/shard_role_api/shard_role_mock.h"

namespace mongo {

namespace shard_role_mock {

CollectionAcquisition acquireCollectionMocked(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              CollectionPtr collptr) {
    auto& txnResources = shard_role_details::TransactionResources::get(opCtx);
    const auto dbLockMode = MODE_IS;
    auto dbLock = std::make_shared<Lock::DBLock>(opCtx, nss.dbName(), dbLockMode);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    shard_role_details::AcquisitionLocks lockRequirements;
    lockRequirements.dbLock = MODE_IS;
    lockRequirements.collLock = MODE_IS;
    auto prerequisites =
        AcquisitionPrerequisites{nss,
                                 boost::none,
                                 repl::ReadConcernArgs::get(opCtx),
                                 PlacementConcern::kPretendUnsharded,
                                 AcquisitionPrerequisites::kRead,
                                 AcquisitionPrerequisites::ViewMode::kMustBeCollection};
    auto currentAcquireCallNum = txnResources.increaseAcquireCollectionCallCount();
    shard_role_details::AcquiredCollection& acquiredCollection =
        txnResources.addAcquiredCollection({currentAcquireCallNum,
                                            prerequisites,
                                            std::move(dbLock),
                                            std::move(collLock),
                                            std::move(lockRequirements),
                                            std::move(collptr)});
    const auto mockedAcquisition = CollectionAcquisition(txnResources, acquiredCollection);
    return mockedAcquisition;
}

CollectionAcquisition acquireCollectionMocked(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              ConsistentCollection collection) {
    auto collptr = CollectionPtr(collection);
    return acquireCollectionMocked(opCtx, nss, std::move(collptr));
}
}  // namespace shard_role_mock
}  // namespace mongo
