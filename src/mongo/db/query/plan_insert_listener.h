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

#pragma once

#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"

namespace mongo::insert_listener {
/**
 * A helper wrapper struct around CappedInsertNotifier which also holds the last version returned
 * by the 'notifier'.
 */
struct CappedInsertNotifierData {
    std::shared_ptr<CappedInsertNotifier> notifier;
    uint64_t lastEOFVersion = ~0;
};

/**
 * Returns true if the PlanExecutor should listen for inserts, which is when a getMore is called
 * on a tailable and awaitData cursor that still has time left and hasn't been interrupted.
 */

bool shouldListenForInserts(OperationContext* opCtx, CanonicalQuery* cq);

/**
 * Returns true if the PlanExecutor should wait for data to be inserted, which is when a getMore
 * is called on a tailable and awaitData cursor on a capped collection.  Returns false if an EOF
 * should be returned immediately.
 */
bool shouldWaitForInserts(OperationContext* opCtx,
                          CanonicalQuery* cq,
                          PlanYieldPolicy* yieldPolicy);

/**
 * Gets the CappedInsertNotifier for a capped collection.  Returns nullptr if this plan executor
 * is not capable of yielding based on a notifier.
 */
std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              PlanYieldPolicy* yieldPolicy);

/**
 * Called for tailable and awaitData cursors in order to yield locks and waits for inserts to
 * the collection being tailed. Returns control to the caller once there has been an insertion
 * and there may be new results. If the PlanExecutor was killed during a yield, throws an
 * exception.
 */

void waitForInserts(OperationContext* opCtx,
                    PlanYieldPolicy* yieldPolicy,
                    CappedInsertNotifierData* notifierData);
}  // namespace mongo::insert_listener
