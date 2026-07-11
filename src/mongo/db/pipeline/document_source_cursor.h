// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
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
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

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
    static constexpr std::string_view kStageName = "$cursor"sv;

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

    DocumentSourceCursor(std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         CursorType cursorType,
                         ResumeTrackingType resumeTrackingType = ResumeTrackingType::kNone);

    ~DocumentSourceCursor() override;

    /**
     * Create a document source based on a passed-in PlanExecutor. 'exec' must be a yielding
     * PlanExecutor, and must be registered with the associated collection's CursorManager.
     *
     * If 'cursorType' is 'kEmptyDocuments', then we inform the $cursor stage that this is a
     * count scenario -- the dependency set is fully known and is empty. In this case, the newly
     * created $cursor stage can return a sequence of empty documents for the caller to count.
     */
    static boost::intrusive_ptr<DocumentSourceCursor> create(
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        CursorType cursorType,
        ResumeTrackingType resumeTrackingType = ResumeTrackingType::kNone);

    void bindCatalogInfo(
        const MultipleCollectionAccessor& collections,
        boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> stasher) override;

    std::string_view getSourceName() const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
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

    void setCatalogResourceHandle_forTest(boost::intrusive_ptr<CatalogResourceHandle> handle) {
        _catalogResourceHandle = std::move(handle);
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceCursorToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceGeoNearCursorToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    static constexpr std::string_view toString(CursorType type) {
        switch (type) {
            case CursorType::kRegular:
                return "regular"sv;
            case CursorType::kEmptyDocuments:
                return "emptyDocuments"sv;
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

class DSCursorCatalogResourceHandle : public DSCatalogResourceHandleBase {
public:
    using DSCatalogResourceHandleBase::DSCatalogResourceHandleBase;

    void checkCanServeReads(OperationContext* opCtx, const PlanExecutor& exec) override {
        if (!shard_role_details::TransactionResources::get(opCtx).isEmpty()) {
            uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(
                opCtx, exec.nss(), true));
        }
    }
};

}  // namespace mongo
