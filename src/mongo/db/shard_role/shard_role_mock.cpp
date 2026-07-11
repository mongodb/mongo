// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_role_mock.h"

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
