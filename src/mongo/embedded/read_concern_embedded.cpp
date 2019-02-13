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

#include "mongo/db/read_concern.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/speculative_majority_read_info.h"

namespace mongo {

MONGO_REGISTER_SHIM(waitForReadConcern)
(OperationContext* opCtx, const repl::ReadConcernArgs& readConcernArgs, bool allowAfterClusterTime)
    ->Status {
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
MONGO_REGISTER_SHIM(waitForSpeculativeMajorityReadConcern)
(OperationContext* opCtx, repl::SpeculativeMajorityReadInfo speculativeReadInfo)->Status {
    return Status::OK();
}

MONGO_REGISTER_SHIM(waitForLinearizableReadConcern)
(OperationContext* opCtx, const int readConcernTimeout)->Status {
    return Status::OK();
}

}  // namespace mongo
