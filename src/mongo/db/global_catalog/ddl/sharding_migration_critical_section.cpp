/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

    if (op == kWrite || _critSecCtx->readsShouldWaitOnCritSec)
        return _critSecCtx->critSecSignal.getFuture();

    return boost::none;
}

boost::optional<BSONObj> ShardingMigrationCriticalSection::getReason() const {
    if (!_critSecCtx)
        return boost::none;

    return _critSecCtx->reason;
}

}  // namespace mongo
