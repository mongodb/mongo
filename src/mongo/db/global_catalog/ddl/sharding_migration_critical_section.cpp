// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
std::string getMessageMismatchReason(const std::string& action,
                                     const BSONObj& newReason,
                                     const BSONObj& existingReason) {
    return str::stream() << "trying to " << action << " a critical section with reason "
                         << newReason
                         << " but it was already taken by another operation with different reason "
                         << existingReason << ".";
}

std::string getMessageNotAcquired(const std::string& action, const BSONObj& newReason) {
    return str::stream() << "Trying to " << action << " a critical section with reason "
                         << newReason << " but it was not acquired first.";
}
}  // namespace

ShardingMigrationCriticalSection::ShardingMigrationCriticalSection() = default;

ShardingMigrationCriticalSection::~ShardingMigrationCriticalSection() {
    invariant(!_critSecCtx);
}

void ShardingMigrationCriticalSection::enterCriticalSectionCatchUpPhase(const BSONObj& reason) {
    if (_critSecCtx && _critSecCtx->reason.woCompare(reason) == 0)
        return;

    invariant(!_critSecCtx, getMessageMismatchReason("acquire", reason, _critSecCtx->reason));

    _critSecCtx.emplace(reason.getOwned());
}

void ShardingMigrationCriticalSection::enterCriticalSectionCommitPhase(const BSONObj& reason) {
    invariant(_critSecCtx, getMessageNotAcquired("promote", reason));
    invariant(_critSecCtx->reason.woCompare(reason) == 0,
              getMessageMismatchReason("promote", reason, _critSecCtx->reason));

    _critSecCtx->readsShouldWaitOnCritSec = true;
}

void ShardingMigrationCriticalSection::exitCriticalSection(const BSONObj& reason) {
    invariant(!_critSecCtx || _critSecCtx->reason.woCompare(reason) == 0,
              getMessageMismatchReason("release", reason, _critSecCtx->reason));

    exitCriticalSectionNoChecks();
}

void ShardingMigrationCriticalSection::exitCriticalSectionNoChecks() {
    if (!_critSecCtx)
        return;

    _critSecCtx->critSecSignal.emplaceValue();
    _critSecCtx.reset();
}

void ShardingMigrationCriticalSection::rollbackCriticalSectionCommitPhaseToCatchUpPhase(
    const BSONObj& reason) {
    invariant(_critSecCtx, getMessageNotAcquired("rollbackToCatchUp", reason));
    invariant(_critSecCtx->reason.woCompare(reason) == 0,
              getMessageMismatchReason("rollbackToCatchUp", reason, _critSecCtx->reason));

    _critSecCtx->readsShouldWaitOnCritSec = false;
}

boost::optional<SharedSemiFuture<void>> ShardingMigrationCriticalSection::getSignal(
    Operation op) const {
    if (!_critSecCtx)
        return boost::none;

    if (op == kWrite || _critSecCtx->readsShouldWaitOnCritSec) {
        return _critSecCtx->critSecSignal.getFuture();
    }

    return boost::none;
}

boost::optional<BSONObj> ShardingMigrationCriticalSection::getReason() const {
    if (!_critSecCtx)
        return boost::none;

    return _critSecCtx->reason;
}

}  // namespace mongo
