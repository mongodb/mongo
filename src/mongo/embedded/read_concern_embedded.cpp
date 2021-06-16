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

#include "mongo/base/shim.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/speculative_majority_read_info.h"

namespace mongo {
namespace {

void setPrepareConflictBehaviorForReadConcernImpl(
    OperationContext* opCtx,
    const repl::ReadConcernArgs& readConcernArgs,
    PrepareConflictBehavior requestedPrepareConflictBehavior) {}

Status waitForReadConcernImpl(OperationContext* opCtx,
                              const repl::ReadConcernArgs& readConcernArgs,
                              StringData dbName,
                              bool allowAfterClusterTime) {
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern) {
        return {ErrorCodes::NotImplemented, "linearizable read concern not supported on embedded"};
    } else if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        return {ErrorCodes::NotImplemented, "snapshot read concern not supported on embedded"};
    } else if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
        return {ErrorCodes::NotImplemented, "majority read concern not supported on embedded"};
    } else if (readConcernArgs.getArgsAfterClusterTime()) {
        return {ErrorCodes::NotImplemented, "afterClusterTime is not supported on embedded"};
    } else if (readConcernArgs.getArgsOpTime()) {
        return {ErrorCodes::NotImplemented, "afterOpTime is not supported on embedded"};
    }

    return Status::OK();
}

Status waitForSpeculativeMajorityReadConcernImpl(
    OperationContext* opCtx, repl::SpeculativeMajorityReadInfo speculativeReadInfo) {
    return Status::OK();
}

Status waitForLinearizableReadConcernImpl(OperationContext* opCtx, int readConcernTimeout) {
    return Status::OK();
}

auto setPrepareConflictBehaviorForReadConcernRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    setPrepareConflictBehaviorForReadConcern, setPrepareConflictBehaviorForReadConcernImpl);
auto waitForReadConcernRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(waitForReadConcern, waitForReadConcernImpl);
auto waitForSpeculativeMajorityReadConcernRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    waitForSpeculativeMajorityReadConcern, waitForSpeculativeMajorityReadConcernImpl);
auto waitForLinearizableReadConcernRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    waitForLinearizableReadConcern, waitForLinearizableReadConcernImpl);

}  // namespace
}  // namespace mongo
