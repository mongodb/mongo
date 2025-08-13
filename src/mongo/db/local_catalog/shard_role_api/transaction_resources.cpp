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

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <ostream>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const PlacementConcern PlacementConcern::kPretendUnsharded =
    PlacementConcern{boost::none, boost::none, true};

namespace shard_role_details {
namespace {

auto getTransactionResources = OperationContext::declareDecoration<
    std::unique_ptr<shard_role_details::TransactionResources>>();

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

void makeLockerOnOperationContext(OperationContext* opCtx) {
    opCtx->setLockState_DO_NOT_USE(std::make_unique<Locker>(opCtx->getServiceContext()));
}

std::unique_ptr<Locker> swapLocker(OperationContext* opCtx,
                                   std::unique_ptr<Locker> newLocker,
                                   ClientLock& clientLock) {
    return opCtx->swapLockState_DO_NOT_USE(std::move(newLocker), clientLock);
}

std::unique_ptr<Locker> swapLocker(OperationContext* opCtx, std::unique_ptr<Locker> newLocker) {
    ClientLock lk(opCtx->getClient());
    return swapLocker(opCtx, std::move(newLocker), lk);
}

std::unique_ptr<RecoveryUnit> releaseRecoveryUnit(OperationContext* opCtx, ClientLock& clientLock) {
    return opCtx->releaseRecoveryUnit_DO_NOT_USE(clientLock);
}

std::unique_ptr<RecoveryUnit> releaseRecoveryUnit(OperationContext* opCtx) {
    ClientLock lk(opCtx->getClient());
    return releaseRecoveryUnit(opCtx, lk);
}

std::unique_ptr<RecoveryUnit> releaseAndReplaceRecoveryUnit(OperationContext* opCtx,
                                                            ClientLock& clientLock) {
    return opCtx->releaseAndReplaceRecoveryUnit_DO_NOT_USE(clientLock);
}

WriteUnitOfWork::RecoveryUnitState setRecoveryUnit(OperationContext* opCtx,
                                                   std::unique_ptr<RecoveryUnit> unit,
                                                   WriteUnitOfWork::RecoveryUnitState state,
                                                   ClientLock& clientLock) {
    return opCtx->setRecoveryUnit_DO_NOT_USE(std::move(unit), state, clientLock);
}

WriteUnitOfWork::RecoveryUnitState setRecoveryUnit(OperationContext* opCtx,
                                                   std::unique_ptr<RecoveryUnit> unit,
                                                   WriteUnitOfWork::RecoveryUnitState state) {
    ClientLock lk(opCtx->getClient());
    return setRecoveryUnit(opCtx, std::move(unit), state, lk);
}

WriteUnitOfWork* getWriteUnitOfWork(OperationContext* opCtx) {
    return opCtx->getWriteUnitOfWork_DO_NOT_USE();
}

void setWriteUnitOfWork(OperationContext* opCtx, std::unique_ptr<WriteUnitOfWork> writeUnitOfWork) {
    opCtx->setWriteUnitOfWork_DO_NOT_USE(std::move(writeUnitOfWork));
}

TransactionResources::TransactionResources() = default;

TransactionResources::~TransactionResources() {
    invariant(!locker);
    invariant(acquiredCollections.empty());
    invariant(acquiredViews.empty());
    invariant(collectionAcquisitionReferences == 0);
    invariant(viewAcquisitionReferences == 0);
    invariant(!yielded);
}

TransactionResources& TransactionResources::get(OperationContext* opCtx) {
    auto& transactionResources = getTransactionResources(opCtx);
    invariant(transactionResources,
              "Cannot obtain TransactionResources as they've been detached from the opCtx in order "
              "to yield");
    return *transactionResources;
}

bool TransactionResources::isPresent(OperationContext* opCtx) {
    return bool(getTransactionResources(opCtx));
}

std::unique_ptr<TransactionResources> TransactionResources::detachFromOpCtx(
    OperationContext* opCtx) {
    auto& transactionResources = getTransactionResources(opCtx);
    invariant(transactionResources);
    return std::move(transactionResources);
}

void TransactionResources::attachToOpCtx(
    OperationContext* opCtx, std::unique_ptr<TransactionResources> newTransactionResources) {
    auto& transactionResources = getTransactionResources(opCtx);
    invariant(!transactionResources);
    transactionResources = std::move(newTransactionResources);
}

AcquiredCollection& TransactionResources::addAcquiredCollection(
    AcquiredCollection&& acquiredCollection) {
    if (!readConcern) {
        readConcern = acquiredCollection.prerequisites.readConcern;
    }

    invariant(state != State::FAILED, "Cannot make a new acquisition in the FAILED state");
    invariant(state != State::YIELDED, "Cannot make a new acquisition in the YIELDED state");

    assertReadConcernsAreEquivalent(*readConcern, acquiredCollection.prerequisites.readConcern);

    if (state == State::EMPTY) {
        state = State::ACTIVE;
    }

    return acquiredCollections.emplace_back(std::move(acquiredCollection));
}

const AcquiredView& TransactionResources::addAcquiredView(AcquiredView&& acquiredView) {
    invariant(state != State::FAILED, "Cannot make a new acquisition in the FAILED state");
    invariant(state != State::YIELDED, "Cannot make a new acquisition in the YIELDED state");

    if (state == State::EMPTY) {
        state = State::ACTIVE;
    }

    return acquiredViews.emplace_back(std::move(acquiredView));
}

void TransactionResources::releaseAllResourcesOnCommitOrAbort() noexcept {
    readConcern.reset();
    acquiredCollections.clear();
    acquiredViews.clear();
    locker.reset();
    yielded.reset();
    catalogEpoch.reset();
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
