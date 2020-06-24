/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <queue>

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/sbe_stage_builder.h"

namespace mongo {
class PlanExecutorSBE final : public PlanExecutor {
public:
    PlanExecutorSBE(
        OperationContext* opCtx,
        std::unique_ptr<CanonicalQuery> cq,
        std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
        NamespaceString nss,
        bool isOpen,
        boost::optional<std::queue<std::pair<BSONObj, boost::optional<RecordId>>>> stash,
        std::unique_ptr<PlanYieldPolicySBE> yieldPolicy);

    PlanStage* getRootStage() const override {
        return nullptr;
    }

    CanonicalQuery* getCanonicalQuery() const override {
        return _cq.get();
    }

    const NamespaceString& nss() const override {
        return _nss;
    }

    OperationContext* getOpCtx() const override {
        return _opCtx;
    }

    void saveState();
    void restoreState();

    void detachFromOperationContext();
    void reattachToOperationContext(OperationContext* opCtx);

    ExecState getNext(BSONObj* out, RecordId* dlOut) override;
    ExecState getNextDocument(Document* objOut, RecordId* dlOut) override;

    bool isEOF() override {
        return _state == State::kClosed;
    }

    void executePlan() override {
        MONGO_UNREACHABLE;
    }

    void markAsKilled(Status killStatus);

    void dispose(OperationContext* opCtx);

    void enqueue(const BSONObj& obj);

    bool isMarkedAsKilled() const override {
        return !_killStatus.isOK();
    }

    Status getKillStatus() override {
        invariant(isMarkedAsKilled());
        return _killStatus;
    }

    bool isDisposed() const override {
        return !_root;
    }

    Timestamp getLatestOplogTimestamp() const override;
    BSONObj getPostBatchResumeToken() const override;

    LockPolicy lockPolicy() const override {
        return LockPolicy::kLocksInternally;
    }

    bool isPipelineExecutor() const override {
        return false;
    }

private:
    enum class State { kClosed, kOpened };

    State _state{State::kClosed};

    OperationContext* _opCtx;

    NamespaceString _nss;

    std::unique_ptr<sbe::PlanStage> _root;

    sbe::value::SlotAccessor* _result{nullptr};
    sbe::value::SlotAccessor* _resultRecordId{nullptr};
    sbe::value::SlotAccessor* _oplogTs{nullptr};
    bool _shouldTrackLatestOplogTimestamp{false};
    bool _shouldTrackResumeToken{false};

    std::queue<std::pair<BSONObj, boost::optional<RecordId>>> _stash;

    // If _killStatus has a non-OK value, then we have been killed and the value represents the
    // reason for the kill.
    Status _killStatus = Status::OK();

    std::unique_ptr<CanonicalQuery> _cq;

    std::unique_ptr<PlanYieldPolicySBE> _yieldPolicy;
};

/**
 * Executes getNext() on the 'root' PlanStage and used 'resultSlot' and 'recordIdSlot' to access the
 * fetched document and it's record id, which are stored in 'out' and 'dlOut' parameters
 * respectively, if they not null pointers.
 *
 * This common logic can be used by various consumers which need to fetch data using an SBE
 * PlanStage tree, such as PlanExecutor or RuntimePlanner.
 */
sbe::PlanState fetchNext(sbe::PlanStage* root,
                         sbe::value::SlotAccessor* resultSlot,
                         sbe::value::SlotAccessor* recordIdSlot,
                         BSONObj* out,
                         RecordId* dlOut);
}  // namespace mongo
