/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_yield_policy_sbe.h"

namespace mongo {

// TODO SERVER-48616: Need to change how we bump the CurOp yield counter. We shouldn't do it here
// so that SBE does not depend on CurOp. But we should still expose that statistic and keep it fresh
// as the operation executes.
Status PlanYieldPolicySBE::yield(OperationContext* opCtx, std::function<void()> whileYieldingFn) {
    if (!_rootStage) {
        // This yield policy isn't bound to an execution tree yet.
        return Status::OK();
    }

    try {
        _rootStage->saveState();

        opCtx->recoveryUnit()->abandonSnapshot();

        if (whileYieldingFn) {
            whileYieldingFn();
        }

        _rootStage->restoreState();
    } catch (...) {
        return exceptionToStatus();
    }

    return Status::OK();
}
}  // namespace mongo
