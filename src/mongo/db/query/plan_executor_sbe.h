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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <deque>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/optimizer/explain_interface.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/restore_context.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/sbe_runtime_planner.h"
#include "mongo/db/query/sbe_stage_builder_plan_data.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

namespace mongo {
class PlanExecutorSBE final : public PlanExecutor {
public:
    struct MetaDataAccessor {
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
                    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                    std::unique_ptr<optimizer::AbstractABTPrinter> optimizerData,
                    sbe::CandidatePlans candidates,
                    bool returnOwnedBson,
                    NamespaceString nss,
                    bool isOpen,
                    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
                    bool generatedByBonsai,
                    boost::optional<size_t> cachedPlanHash,
                    OptimizerCounterInfo optCounterInfo = {},
                    std::unique_ptr<RemoteCursorMap> remoteCursors = nullptr,
                    std::unique_ptr<RemoteExplainVector> remoteExplains = nullptr);

    CanonicalQuery* getCanonicalQuery() const override {
        return _cq.get();
    }

    Pipeline* getPipeline() const override {
        return _pipeline.get();
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

    void saveState();
    void restoreState(const RestoreContext& context);

    void detachFromOperationContext();
    void reattachToOperationContext(OperationContext* opCtx);

    ExecState getNext(BSONObj* out, RecordId* dlOut) override;
    ExecState getNextDocument(Document* objOut, RecordId* dlOut) override;

    bool isEOF() override {
        return isMarkedAsKilled() || (_stash.empty() && _root->getCommonStats()->isEOF);
    }

    long long executeCount() override {
        // Using SBE to execute a count command is not yet supported.
        MONGO_UNREACHABLE;
    }

    UpdateResult executeUpdate() override {
        // Using SBE to execute an update command is not yet supported.
        MONGO_UNREACHABLE;
    }
    UpdateResult getUpdateResult() const override {
        // Using SBE to execute an update command is not yet supported.
        MONGO_UNREACHABLE;
    }

    long long executeDelete() override {
        // Using SBE to execute a delete command is not yet supported.
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

    void markAsKilled(Status killStatus);

    void dispose(OperationContext* opCtx);

    void stashResult(const BSONObj& obj);

    bool isMarkedAsKilled() const override {
        return !_killStatus.isOK();
    }

    Status getKillStatus() override {
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

    void enableSaveRecoveryUnitAcrossCommandsIfSupported() override {
        _isSaveRecoveryUnitAcrossCommandsEnabled = true;
    }
    bool isSaveRecoveryUnitAcrossCommandsEnabled() const override {
        return _isSaveRecoveryUnitAcrossCommandsEnabled;
    }

    PlanExecutor::QueryFramework getQueryFramework() const override final {
        return _generatedByBonsai ? PlanExecutor::QueryFramework::kCQF
                                  : PlanExecutor::QueryFramework::kSBEOnly;
    }

    void setReturnOwnedData(bool returnOwnedData) override final {
        _mustReturnOwnedBson = returnOwnedData;
    }

    bool usesCollectionAcquisitions() const override final;

    /**
     * For queries that have multiple executors, this can be used to differentiate between them.
     */
    boost::optional<StringData> getExecutorType() const override final {
        return CursorType_serializer(_cursorType);
    }

private:
    template <typename ObjectType>
    ExecState getNextImpl(ObjectType* out, RecordId* dlOut);

    void initializeAccessors(MetaDataAccessor& accessor,
                             const stage_builder::PlanStageMetadataSlots& metadataSlots,
                             const QueryMetadataBitSet& metadataBit);

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

    // For parameterized queries generated by Bonsai, we transfer ownership of the pipeline to the
    // plan executor. This is to allow the slots in the runtime environment containing parameterized
    // constants to be views over the MatchExpression contained in the pipeline; this allows us to
    // avoid potentially expensive copies when binding constants into the plan. We also use this
    // pipeline for explain purposes.
    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;

    std::unique_ptr<PlanYieldPolicySBE> _yieldPolicy;

    std::unique_ptr<PlanExplainer> _planExplainer;

    bool _isDisposed{false};

    bool _isSaveRecoveryUnitAcrossCommandsEnabled = false;

    // Indicates whether this executor was constructed via Bonsai/CQF.
    bool _generatedByBonsai{false};

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
 */
sbe::PlanState fetchNext(sbe::PlanStage* root,
                         sbe::value::SlotAccessor* resultSlot,
                         sbe::value::SlotAccessor* recordIdSlot,
                         BSONObj* out,
                         RecordId* dlOut,
                         bool returnOwnedBson);
}  // namespace mongo
