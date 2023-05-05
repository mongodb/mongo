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
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::insert_listener {

// An abstract class used to notify on new insert events.
class Notifier {
public:
    virtual ~Notifier(){};

    // Performs the necessary work needed for waiting. Should be called prior calling waitUntil().
    virtual void prepareForWait(OperationContext* opCtx) = 0;

    // Blocks the caller until an insert event is fired or the deadline is hit.
    virtual void waitUntil(OperationContext* opCtx, Date_t deadline) = 0;
};

// Class used to notify listeners of local inserts into the capped collection.
class LocalCappedInsertNotifier final : public Notifier {
public:
    LocalCappedInsertNotifier(std::shared_ptr<CappedInsertNotifier> notifier)
        : _notifier(notifier) {}

    void prepareForWait(OperationContext* opCtx) final {
        invariant(_notifier);
    }

    void waitUntil(OperationContext* opCtx, Date_t deadline) final {
        auto currentVersion = _notifier->getVersion();
        _notifier->waitUntil(opCtx, _lastEOFVersion, deadline);
        _lastEOFVersion = currentVersion;
    }

private:
    std::shared_ptr<CappedInsertNotifier> _notifier;
    uint64_t _lastEOFVersion = ~uint64_t(0);
};

// Class used to notify listeners on majority committed point advancement events.
class MajorityCommittedPointNotifier final : public Notifier {
public:
    MajorityCommittedPointNotifier(repl::OpTime opTime = repl::OpTime())
        : _opTimeToBeMajorityCommitted(opTime) {}

    // Computes the OpTime to wait on by incrementing the current read timestamp.
    void prepareForWait(OperationContext* opCtx) final {
        auto readTs = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
        invariant(readTs);
        _opTimeToBeMajorityCommitted =
            repl::OpTime(*readTs + 1, repl::ReplicationCoordinator::get(opCtx)->getTerm());
    }

    void waitUntil(OperationContext* opCtx, Date_t deadline) final {
        auto majorityCommittedFuture = WaitForMajorityService::get(opCtx->getServiceContext())
                                           .waitUntilMajorityForRead(_opTimeToBeMajorityCommitted,
                                                                     opCtx->getCancellationToken());
        opCtx->runWithDeadline(deadline, opCtx->getTimeoutError(), [&] {
            auto status = majorityCommittedFuture.getNoThrow(opCtx);
            if (!status.isOK()) {
                LOGV2_DEBUG(7455500,
                            3,
                            "Failure waiting for the majority committed event",
                            "status"_attr = status.toString());
            }
        });
    }

private:
    repl::OpTime _opTimeToBeMajorityCommitted;
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
 * Returns an insert notifier for a capped collection.
 */
std::unique_ptr<Notifier> getCappedInsertNotifier(OperationContext* opCtx,
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
                    std::unique_ptr<Notifier>& notifier);
}  // namespace mongo::insert_listener

#undef MONGO_LOGV2_DEFAULT_COMPONENT
