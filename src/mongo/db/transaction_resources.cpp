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

#include "mongo/db/transaction_resources.h"

namespace mongo {

const PlacementConcern AcquisitionPrerequisites::kPretendUnsharded =
    PlacementConcern{boost::none, boost::none};

namespace shard_role_details {
namespace {

/**
 * This method ensures that two read concerns are equivalent for the purposes of acquiring a
 * transactional snapshot. Equivalence means that they don't acquire snapshot at conflicting levels,
 * such as one operation asking for local and a subsequent one for majority. Similarly, we can't
 * have two subsequent acquisitions asking for snapshots at two different timestamps.
 */
void assertReadConcernsAreEquivalent(const repl::ReadConcernArgs& rc1,
                                     const repl::ReadConcernArgs& rc2) {
    tassert(771230,
            str::stream() << "Acquired two different collections on the same transaction with "
                             "read concerns that are not equivalent ("
                          << rc1.toString() << " != " << rc2.toString() << ")",
            rc1.getLevel() == rc2.getLevel() &&
                rc1.getArgsAtClusterTime() == rc2.getArgsAtClusterTime());
}

}  // namespace

TransactionResources::TransactionResources() = default;

TransactionResources::~TransactionResources() {
    invariant(!locker);
    invariant(!yieldedLocker);
    invariant(!yieldedRecoveryUnit);
    invariant(acquiredCollections.empty());
    invariant(acquiredViews.empty());
}

AcquiredCollection& TransactionResources::addAcquiredCollection(
    AcquiredCollection&& acquiredCollection) {
    if (!readConcern) {
        readConcern = acquiredCollection.prerequisites.readConcern;
    }
    assertReadConcernsAreEquivalent(*readConcern, acquiredCollection.prerequisites.readConcern);

    return acquiredCollections.emplace_back(std::move(acquiredCollection));
}

const AcquiredView& TransactionResources::addAcquiredView(AcquiredView&& acquiredView) {
    return acquiredViews.emplace_back(std::move(acquiredView));
}

void TransactionResources::releaseAllResourcesOnCommitOrAbort() noexcept {
    readConcern.reset();
    locker.reset();
    yieldedLocker.reset();
    yieldedRecoveryUnit.reset();
    acquiredCollections.clear();
    acquiredViews.clear();
}

void TransactionResources::assertNoAcquiredCollections() const {
    if (acquiredCollections.empty())
        return;

    std::stringstream ss("Found acquired collections:");
    for (const auto& acquisition : acquiredCollections) {
        ss << "\n" << acquisition.prerequisites.nss.toStringForErrorMsg();
    }
    fassertFailedWithStatus(737660, Status{ErrorCodes::InternalError, ss.str()});
}

}  // namespace shard_role_details
}  // namespace mongo
