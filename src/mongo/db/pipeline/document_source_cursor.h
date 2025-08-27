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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

struct CursorSharedState {
    // The underlying query plan which feeds this pipeline. Must be destroyed while holding the
    // collection lock.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;

    // Status of the underlying executor, exec. Used for explain queries if exec produces an
    // error. Since exec may not finish running (if there is a limit, for example), we store
    // OK as the default.
    Status execStatus;

    // Specific stats for $cursor stage.
    DocumentSourceCursorStats stats;

    // Updated at most once. 'true' if a CursorStage is created.
    bool execStageCreated{false};
};

/**
 * Constructs and returns Documents from the BSONObj objects produced by a supplied
 * PlanExecutor.
 */
class DocumentSourceCursor : public DocumentSource {
public:
    /**
     * Interface for acquiring and releasing catalog resources needed for DS Cursor.
     */
    class CatalogResourceHandle : public RefCountable {
    public:
        virtual void acquire(OperationContext*, const PlanExecutor&) = 0;
        virtual void release() = 0;
        virtual void checkCanServeReads(OperationContext* opCtx, const PlanExecutor& exec) = 0;
    };

    static constexpr StringData kStageName = "$cursor"_sd;

    /**
     * Indicates whether or not this is a count-like operation. If the operation is count-like,
     * then the cursor can produce empty documents since the subsequent stages need only the
     * count of these documents (not the actual data).
     */
    enum class CursorType { kRegular, kEmptyDocuments };

    /**
     * Indicates whether we are tracking resume information from an oplog query (e.g. for
     * change streams), from a non-oplog query (natural order scan using recordId information)
     * or neither.
     */
    enum class ResumeTrackingType { kNone, kOplog, kNonOplog };

    /**
     * Create a document source based on a passed-in PlanExecutor. 'exec' must be a yielding
     * PlanExecutor, and must be registered with the associated collection's CursorManager.
     *
     * If 'cursorType' is 'kEmptyDocuments', then we inform the $cursor stage that this is a
     * count scenario -- the dependency set is fully known and is empty. In this case, the newly
     * created $cursor stage can return a sequence of empty documents for the caller to count.
     */
    static boost::intrusive_ptr<DocumentSourceCursor> create(
        const MultipleCollectionAccessor& collections,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
        const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        CursorType cursorType,
        ResumeTrackingType resumeTrackingType = ResumeTrackingType::kNone);

    const char* getSourceName() const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    void detachSourceFromOperationContext() final;

    void reattachSourceToOperationContext(OperationContext* opCtx) final;

    const PlanExplainer::ExplainVersion& getExplainVersion() const {
        return _sharedState->exec->getPlanExplainer().getVersion();
    }

    boost::optional<const PlanExplainer&> getPlanExplainer() const {
        if (!_sharedState->exec) {
            return boost::none;
        }
        return _sharedState->exec->getPlanExplainer();
    }

    PlanExecutor::QueryFramework getQueryFramework() const {
        return _queryFramework;
    }

    BSONObj serializeToBSONForDebug() const final {
        // Feel free to add any useful information here. For now this has not been useful for
        // debugging so is left empty.
        return BSON(kStageName << "{}");
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        // The assumption is that dependency analysis and non-correlated prefix analysis happens
        // before a $cursor is attached to a pipeline.
        MONGO_UNREACHABLE;
    }

protected:
    DocumentSourceCursor(const MultipleCollectionAccessor& collections,
                         std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                         const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         CursorType cursorType,
                         ResumeTrackingType resumeTrackingType = ResumeTrackingType::kNone);

    ~DocumentSourceCursor() override;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceCursorToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceGeoNearCursorToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    static constexpr StringData toString(CursorType type) {
        switch (type) {
            case CursorType::kRegular:
                return "regular"_sd;
            case CursorType::kEmptyDocuments:
                return "emptyDocuments"_sd;
        }
        MONGO_UNREACHABLE;
    }

    // Handle to catalog state.
    boost::intrusive_ptr<CatalogResourceHandle> _catalogResourceHandle;

    // Used only for explain() queries. Stores the stats of the winning plan when a plan was
    // selected by the multi-planner. When the query is executed (with exec->executePlan()), it
    // will wipe out its own copy of the winning plan's statistics, so they need to be saved
    // here.
    boost::optional<PlanExplainer::PlanStatsDetails> _winningPlanTrialStats;

    // Whether we are tracking the latest observed oplog timestamp, the resume token from the
    // (non-oplog) scan, or neither.
    ResumeTrackingType _resumeTrackingType = ResumeTrackingType::kNone;

    // Whether or not this is a count-like operation (the cursor can then produce empty documents
    // since the subsequent stages need only the count of these documents).
    CursorType _cursorType;

    PlanExecutor::QueryFramework _queryFramework;

    boost::optional<Explain::PlannerContext> _plannerContext;

    const std::shared_ptr<CursorSharedState> _sharedState;
};

class DSCursorCatalogResourceHandle : public DocumentSourceCursor::CatalogResourceHandle {
public:
    DSCursorCatalogResourceHandle(
        boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> stasher)
        : _transactionResourcesStasher(std::move(stasher)) {
        tassert(10096101,
                "Expected _transactionResourcesStasher to exist",
                _transactionResourcesStasher);
    }

    void acquire(OperationContext* opCtx, const PlanExecutor& exec) override {
        tassert(10271302, "Expected resources to be absent", !_resources);
        _resources.emplace(opCtx, _transactionResourcesStasher.get());
    }

    void release() override {
        _resources.reset();
    }

    void checkCanServeReads(OperationContext* opCtx, const PlanExecutor& exec) override {
        if (!shard_role_details::TransactionResources::get(opCtx).isEmpty()) {
            uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(
                opCtx, exec.nss(), true));
        }
    }

private:
    boost::optional<HandleTransactionResourcesFromStasher> _resources;
    boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline>
        _transactionResourcesStasher;
};

}  // namespace mongo
