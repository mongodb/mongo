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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role.h"

#include "mongo/util/fail_point.h"

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(planExecutorAlwaysFails);
}  // namespace

const OperationContext::Decoration<boost::optional<SharedSemiFuture<void>>>
    planExecutorShardingCriticalSectionFuture =
        OperationContext::declareDecoration<boost::optional<SharedSemiFuture<void>>>();

std::string PlanExecutor::stateToStr(ExecState execState) {
    switch (execState) {
        case PlanExecutor::ADVANCED:
            return "ADVANCED";
        case PlanExecutor::IS_EOF:
            return "IS_EOF";
    }
    MONGO_UNREACHABLE;
}

void PlanExecutor::checkFailPointPlanExecAlwaysFails() {
    if (MONGO_unlikely(planExecutorAlwaysFails.shouldFail())) {
        uasserted(ErrorCodes::Error(4382101),
                  "PlanExecutor hit planExecutorAlwaysFails fail point");
    }
}

const CollectionPtr& VariantCollectionPtrOrAcquisition::getCollectionPtr() const {
    return *stdx::visit(OverloadedVisitor{
                            [](const CollectionPtr* collectionPtr) { return collectionPtr; },
                            [](const ScopedCollectionAcquisition* collectionAcquisition) {
                                return &collectionAcquisition->getCollectionPtr();
                            },
                        },
                        _collectionPtrOrAcquisition);
}

}  // namespace mongo
