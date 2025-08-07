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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/restore_context.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class PlanExecutorSBE final : public PlanExecutor {
public:
    struct MetaDataAccessor {
        template <typename BSONTraits = BSONObj::DefaultSizeTrait>
        BSONObj appendToBson(BSONObj doc) const;
        Document appendToDocument(Document doc) const;
        // Only for $search queries, holds the metadata returned from mongot.
        sbe::value::SlotAccessor* metadataSearchScore{nullptr};
        sbe::value::SlotAccessor* metadataSearchHighlights{nullptr};
        sbe::value::SlotAccessor* metadataSearchDetails{nullptr};
        sbe::value::SlotAccessor* metadataSearchSortValues{nullptr};
        sbe::value::SlotAccessor* metadataSearchSequenceToken{nullptr};

        sbe::value::SlotAccessor* sortKey{nullptr};
        bool isSingleSortKey{true};
    };

    PlanExecutorSBE(OperationContext* opCtx,
                    std::unique_ptr<CanonicalQuery> cq,
                    sbe::plan_ranker::CandidatePlan plan,
                    bool returnOwnedBson,
                    NamespaceString nss,
                    bool isOpen,
                    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
                    boost::optional<size_t> cachedPlanHash,
                    std::unique_ptr<RemoteCursorMap> remoteCursors,
                    std::unique_ptr<RemoteExplainVector> remoteExplains,
                    std::unique_ptr<MultiPlanStage> classicRuntimePlannerStage,
                    const MultipleCollectionAccessor& mca);

    CanonicalQuery* getCanonicalQuery() const override {
        return _cq.get();
    }

    const NamespaceString& nss() const override {
        return _nss;
    }

    const std::vector<NamespaceStringOrUUID>& getSecondaryNamespaces() const final {
        return _secondaryNssVector;
    }

    OperationContext* getOpCtx() const override {
        return _opCtx;
    }

    void saveState() override;
    void restoreState(const RestoreContext& context) override;

    void detachFromOperationContext() override;
    void reattachToOperationContext(OperationContext* opCtx) override;

    ExecState getNext(BSONObj* out, RecordId* dlOut) override;
    ExecState getNextDocument(Document& objOut) override;

    bool isEOF() const override {
        return isMarkedAsKilled() || (_stash.empty() && _root->getCommonStats()->isEOF);
    }

    long long executeCount() override {
        // Using SBE to execute a count command is not yet supported.
        MONGO_UNREACHABLE;
    }

    UpdateResult getUpdateResult() const override {
        // Using SBE to execute an update command is not yet supported.
        MONGO_UNREACHABLE;
    }
    long long getDeleteResult() const override {
        // Using SBE to execute a delete command is not yet supported.
        MONGO_UNREACHABLE;
    }
    BatchedDeleteStats getBatchedDeleteStats() override {
        // Using SBE to execute a batched delete command is not yet supported.
        MONGO_UNREACHABLE;
    }

    void markAsKilled(Status killStatus) override;

    void dispose(OperationContext* opCtx) override;

    void forceSpill(PlanYieldPolicy* yieldPolicy) override {
        _root->forceSpill(yieldPolicy);
    }

    void stashResult(const BSONObj& obj) override;

    bool isMarkedAsKilled() const override {
        return !_killStatus.isOK();
    }

    Status getKillStatus() const override {
        invariant(isMarkedAsKilled());
        return _killStatus;
    }

    bool isDisposed() const override {
        return _isDisposed;
    }

    Timestamp getLatestOplogTimestamp() const override;
    BSONObj getPostBatchResumeToken() const override;

    /**
     * The caller must acquire a top level AutoGet object outside of this PlanExecutor in order to
     * open a storage transaction and establish a consistent view of the catalog.
     */
    LockPolicy lockPolicy() const override {
        return LockPolicy::kLockExternally;
    }

    const PlanExplainer& getPlanExplainer() const final {
        invariant(_planExplainer);
        return *_planExplainer;
    }

    PlanExecutor::QueryFramework getQueryFramework() const final {
        return PlanExecutor::QueryFramework::kSBEOnly;
    }

    void setReturnOwnedData(bool returnOwnedData) final {
        _mustReturnOwnedBson = returnOwnedData;
    }

    bool usesCollectionAcquisitions() const final;

    /**
     * For queries that have multiple executors, this can be used to differentiate between them.
     */
    boost::optional<StringData> getExecutorType() const final {
        return CursorType_serializer(_cursorType);
    }

private:
    template <typename ObjectType>
    ExecState getNextImpl(ObjectType* out, RecordId* dlOut);

    void initializeAccessors(MetaDataAccessor& accessor,
                             const stage_builder::PlanStageMetadataSlots& metadataSlots);

    enum class State { kClosed, kOpened };

    State _state{State::kClosed};

    OperationContext* _opCtx;

    NamespaceString _nss;

    // Vector of secondary namespaces.
    std::vector<NamespaceStringOrUUID> _secondaryNssVector{};
    bool _mustReturnOwnedBson;

    // CompileCtx owns the instance pointed by _env, so we must keep it around.
    const std::unique_ptr<sbe::PlanStage> _root;
    stage_builder::PlanStageData _rootData;
    std::unique_ptr<QuerySolution> _solution;

    sbe::value::SlotAccessor* _result{nullptr};
    sbe::value::SlotAccessor* _resultRecordId{nullptr};
    sbe::value::TypeTags _tagLastRecordId{sbe::value::TypeTags::Nothing};
    sbe::value::Value _valLastRecordId{0};
    sbe::RuntimeEnvironment::Accessor* _oplogTs{nullptr};

    // Only for a resumed scan ("seek"). Slot holding the TypeTags::RecordId of the record to resume
    // the scan from. '_seekRecordId' is the RecordId value, initialized from the slot at runtime.
    boost::optional<sbe::value::SlotId> _resumeRecordIdSlot;

    // Only for clustered collection scans, holds the minimum record ID of the scan, if applicable.
    boost::optional<sbe::value::SlotId> _minRecordIdSlot;

    // Only for clustered collection scans, holds the maximum record ID of the scan, if applicable.
    boost::optional<sbe::value::SlotId> _maxRecordIdSlot;

    MetaDataAccessor _metadataAccessors;

    // NOTE: '_stash' stores documents as BSON. Currently, one of the '_stash' is usages is to store
    // documents received from the plan during multiplanning. This means that the documents
    // generated during multiplanning cannot exceed maximum BSON size. $group and $lookup CAN
    // produce documents larger than maximum BSON size. But $group and $lookup never participate in
    // multiplanning. This is why maximum BSON size limitation in '_stash' is not an issue for such
    // operators.
    // Another usage of '_stash' is when the 'find' command cannot fit the last returned document
    // into the result batch. But in this case each document is already requried to fit into the
    // maximum BSON size, because all results are encoded into BSON before returning to client. So
    // using BSON in '_stash' does not introduce any additional limitations.
    std::deque<std::pair<BSONObj, boost::optional<RecordId>>> _stash;

    // If we are returning owned result (i.e. value is moved out of the result accessor) then its
    // lifetime must extend up to the next getNext (or saveState).
    BSONObj _lastGetNext;

    // If _killStatus has a non-OK value, then we have been killed and the value represents the
    // reason for the kill.
    Status _killStatus = Status::OK();

    std::unique_ptr<CanonicalQuery> _cq;

    std::unique_ptr<PlanYieldPolicySBE> _yieldPolicy;

    std::unique_ptr<PlanExplainer> _planExplainer;

    bool _isDisposed{false};

    /**
     * For commands that return multiple cursors, this value will contain the type of cursor.
     * Default to a regular result cursor.
     */
    CursorTypeEnum _cursorType = CursorTypeEnum::DocumentResult;

    std::unique_ptr<RemoteCursorMap> _remoteCursors;
    std::unique_ptr<RemoteExplainVector> _remoteExplains;
};

/**
 * Executes getNext() on the 'root' PlanStage and used 'resultSlot' and 'recordIdSlot' to access the
 * fetched document and it's record id, which are stored in 'out' and 'dlOut' parameters
 * respectively, if they not null pointers.
 *
 * This common logic can be used by various consumers which need to fetch data using an SBE
 * PlanStage tree, such as PlanExecutor or RuntimePlanner.
 *
 * BSONTraits template parameter can be set to BSONObj::LargeSizeTrait if we want to allow resulting
 * BSONObj to be larged than 16 MB.
 */
template <typename BSONTraits = BSONObj::DefaultSizeTrait>
sbe::PlanState fetchNext(sbe::PlanStage* root,
                         sbe::value::SlotAccessor* resultSlot,
                         sbe::value::SlotAccessor* recordIdSlot,
                         BSONObj* out,
                         RecordId* dlOut,
                         bool returnOwnedBson);
}  // namespace mongo
