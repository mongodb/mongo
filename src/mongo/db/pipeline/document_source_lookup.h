// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_lookup.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

// $lookup re-parses resolvedPipeline BSON per input document and buildPipeline() runs
// makeLookupViewBinder to bind view info onto extension stages at parse time. When
// resolvedPipeline is serialized from an already-parsed pipeline (e.g. hybrid search
// introspection), view binding is already applied.
// Re-binding overwrites already-resolved stages with the user-facing view name.
// kAlreadyBound skips makeLookupViewBinder to prevent this scenario.
enum class LookupResolvedPipelineViewBinding {
    kNeedsBinding,
    kAlreadyBound,
};

struct LookUpSharedState {
    // TODO SERVER-107976: Move 'pipeline' and 'execPipeline' entirely into the 'LookUpStage' class.
    std::unique_ptr<mongo::Pipeline> pipeline;
    std::unique_ptr<exec::agg::Pipeline> execPipeline;

    // The aggregation pipeline to perform against the '_resolvedNs' namespace. Referenced view
    // namespaces have been resolved.
    // TODO SERVER-107976: Make 'resolvedPipeline' a 'std::shared_ptr<std::vector<BSONObj>>' and
    // move it back to the 'DocumentSourceLookUp' class.
    std::vector<BSONObj> resolvedPipeline;

    LookupResolvedPipelineViewBinding resolvedPipelineViewBinding =
        LookupResolvedPipelineViewBinding::kNeedsBinding;

    // A pipeline parsed from _sharedState->resolvedPipeline at creation time, intended to support
    // introspective functions. If sub-$lookup stages are present, their pipelines are constructed
    // recursively.
    // TODO SERVER-107976: Move 'resolvedIntrospectionPipeline' back to the 'DocumentSourceLookUp'
    // class.
    std::unique_ptr<Pipeline> resolvedIntrospectionPipeline;
};

void lookupPipeValidator(const Pipeline& pipeline);

// Parses $lookup's 'from' field. Accepts a string or a '{db, coll}' object with specific
// exceptions for internal namespaces (config.cache.chunks.*, local.oplog.rs,
// config.collections, config.chunks) and when allowGenericForeignDbLookup is set.
NamespaceString parseLookupFromAndResolveNamespace(const BSONElement& elem,
                                                   const DatabaseName& defaultDb,
                                                   bool allowGenericForeignDbLookup);

/**
 * Queries separate collection for equality matches with documents in the pipeline collection.
 * Adds matching documents to a new array field in the input document.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceLookUp final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$lookup"sv;
    static constexpr std::string_view kFromField = "from"sv;
    static constexpr std::string_view kLocalField = "localField"sv;
    static constexpr std::string_view kForeignField = "foreignField"sv;
    static constexpr std::string_view kPipelineField = "pipeline"sv;
    static constexpr std::string_view kAsField = "as"sv;

    /**
     * Copy constructor used for clone().
     */
    DocumentSourceLookUp(const DocumentSourceLookUp&,
                         const boost::intrusive_ptr<ExpressionContext>&);

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void serializeToArray(std::vector<Value>& array,
                          const query_shape::SerializationOptions& opts =
                              query_shape::SerializationOptions{}) const final;

    /**
     * Returns the 'as' path, and possibly fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    void describeTransformation(
        document_transformation::DocumentOperationVisitor& visitor) const override;

    /**
     * Reports the StageConstraints of this $lookup instance. A $lookup constructed with pipeline
     * syntax will inherit certain constraints from the stages in its pipeline.
     */
    StageConstraints constraints(PipelineSplitState) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;

    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachSourceFromOperationContext() final;

    void reattachSourceToOperationContext(OperationContext* opCtx) final;

    bool validateSourceOperationContext(const OperationContext* opCtx) const final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    // Build a $lookup from pre-parsed StageParams. Performs expCtx-dependent validation
    // (cross-db on mongos / view definition, hybrid-search timeseries) and calls the
    // StageParams-accepting constructor when a subpipeline is present.
    static DocumentSourceContainer createFromStageParams(
        LookUpStageParams& params, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Constructor accepting pre-parsed StageParams for the subpipeline. Avoids the per-construction
     * re-parse of the subpipeline's BSON that createFromBson does. Used by createFromStageParams
     * when LookUpStageParams::subpipelineStageParams is present.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::vector<BSONObj> userPipeline,
                         StageParamsPipeline subpipelineStageParams,
                         BSONObj letVariables,
                         boost::optional<std::pair<std::string, std::string>> localForeignFields,
                         boost::optional<BSONObj> unwindSpec,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         bool containsUserSpecifiedPipeline = true);

    static std::unique_ptr<Pipeline> parsePipelineFromStageParamsWithMaybeViewDefinition(
        const boost::intrusive_ptr<ExpressionContext>& fromExpCtx,
        const ResolvedNamespace& resolvedNs,
        StageParamsPipeline stageParams,
        const std::vector<BSONObj>& rawPipeline,
        const NamespaceString& fromNss);

    void resolvedPipelineHelper(
        NamespaceString fromNs,
        std::vector<BSONObj> pipeline,
        boost::optional<std::pair<std::string, std::string>> localForeignFields,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Builds the BSONObj used to query the foreign collection and wraps it in a $match.
     */
    static BSONObj makeMatchStageFromInput(const Document& input,
                                           const FieldPath& localFieldName,
                                           const std::string& foreignFieldName,
                                           const BSONObj& additionalFilter);

    /**
     * Helper to absorb an $unwind stage. Only used for testing this special behavior.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] void setUnwindStage_forTest(
        const boost::intrusive_ptr<DocumentSourceUnwind>& unwind) {
        invariant(!_unwindSrc);
        _unwindSrc = unwind;
    }

    bool hasLocalFieldForeignFieldJoin() const {
        return _localField != boost::none;
    }

    bool hasPipeline() const {
        return _userPipeline != boost::none;
    }

    boost::optional<FieldPath> getForeignField() const {
        return _foreignField;
    }

    boost::optional<FieldPath> getLocalField() const {
        return _localField;
    }

    /**
     * "as" field must be present in all types of $lookup queries.
     */
    const FieldPath& getAsField() const {
        return _as;
    }

    const std::vector<LetVariable>& getLetVariables() const {
        return _letVariables;
    }

    /**
     * Returns a non-executable pipeline which can be useful for introspection. In this pipeline,
     * all view definitions are resolved. This pipeline is present in both the sub-pipeline version
     * of $lookup and the simpler 'localField/foreignField' version, but because it is not tied to
     * any document to look up it is missing variable definitions for the former type and the $match
     * stage which will be added to enforce the join criteria for the latter.
     */
    inline const auto& getResolvedIntrospectionPipeline() const {
        return *_sharedState->resolvedIntrospectionPipeline;
    }

    inline auto& getResolvedIntrospectionPipeline() {
        return *_sharedState->resolvedIntrospectionPipeline;
    }

    inline const Variables& getVariables_forTest() {
        return _variables;
    }

    inline const VariablesParseState& getVariablesParseState_forTest() {
        return _variablesParseState;
    }

    inline const std::vector<BSONObj>& getResolvedPipelineForTest() const {
        return _sharedState->resolvedPipeline;
    }

    const DocumentSourceContainer* getSubPipeline() const final {
        tassert(6080015,
                "$lookup expected to have a resolved pipeline, but didn't",
                _sharedState->resolvedIntrospectionPipeline);
        return &_sharedState->resolvedIntrospectionPipeline->getSources();
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final;

    const NamespaceString& getFromNs() const {
        return _fromNs;
    }

    inline const boost::intrusive_ptr<DocumentSourceUnwind>& getUnwindSource() const {
        return _unwindSrc;
    }

    /*
     * Indicates whether this $lookup stage has absorbed an immediately following $unwind stage that
     * unwinds the lookup result array.
     */
    bool hasUnwindSrc() const {
        return bool(_unwindSrc);
    }

    /**
     * True when the foreign namespace was a view at parse time, or when the mongos-to-shard
     * rewrite of 'from' to the view's backing collection carried the bit through
     * $_internalFromIsAView. Read by sbe_pushdown.cpp to refuse lowering $lookup against view
     * foreigns (including rename-only identity views where the pre-existing
     * 'pipeline.empty()' proxy can't tell the foreign was a view).
     */
    bool fromNsIsAView() const {
        return _fromNsIsAView;
    }

    /**
     * Rebuilds the _sharedState->resolvedPipeline from the
     * _sharedState->resolvedIntrospectionPipeline. This is required for server rewrites for FLE2.
     * The server rewrite code operates on DocumentSources of a parsed pipeline, which we obtain
     * from DocumentSourceLookUp::_sharedState->resolvedIntrospectionPipeline. However, we use
     * _sharedState->resolvedPipeline to execute each iteration of doGetNext(). This method is
     * called exclusively from rewriteLookUp (server_rewrite.cpp) once the pipeline has been
     * rewritten for FLE2.
     */
    void rebuildResolvedPipeline();

    /**
     * Returns the expression context associated with foreign collection namespace and/or
     * sub-pipeline.
     */
    boost::intrusive_ptr<ExpressionContext> getSubpipelineExpCtx() const final {
        return _fromExpCtx;
    }

    BSONObj getAdditionalFilter() const {
        return hasPipeline() ? BSONObj() : _additionalFilter.value_or(BSONObj());
    }

    /**
     * Returns the absorbed filter regardless of hasPipeline() - unlike getAdditionalFilter(),
     * which returns {} when hasPipeline() is true to avoid the execution layer double-applying
     * the filter via resolvedPipeline. Safe only for callers that consume the filter
     * independently of resolvedPipeline (e.g. the join optimizer, which builds its own
     * foreign CanonicalQuery).
     */
    BSONObj getAbsorbedFilter() const {
        return _additionalFilter.value_or(BSONObj());
    }

    bool hasAdditionalFilter() const {
        return _additionalFilter.has_value();
    }

    /**
     * Attempts to combine with an immediately following $unwind stage that unwinds the $lookup's
     * "as" field, setting the '_unwindSrc' member to the absorbed $unwind stage. If
     * this is done it may also absorb one or more $match stages that immediately followed the
     * $unwind, setting the resulting combined $match in the '_matchSrc' member.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

protected:
    boost::optional<ShardId> computeMergeShardId() const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceLookUpToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    static void relocateFieldMatchPlaceholder(
        boost::intrusive_ptr<DocumentSourceLookUp>& lookupStage, size_t newIdx);

    /**
     * Target constructor. Handles common-field initialization for the syntax-specific delegating
     * constructors.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx);
    /**
     * Constructor used for a $lookup stage specified using the {from: ..., localField: ...,
     * foreignField: ..., as: ...} syntax.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::string localField,
                         std::string foreignField,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Constructor used for a $lookup stage specified using the pipeline syntax {from: ...,
     * pipeline: [...], as: ...} or using both the localField/foreignField syntax and pipeline
     * syntax: {from: ..., localField: ..., foreignField: ..., pipeline: [...], as: ...}
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::vector<BSONObj> pipeline,
                         BSONObj letVariables,
                         boost::optional<std::pair<std::string, std::string>> localForeignFields,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Should not be called; use serializeToArray instead.
     */
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final {
        MONGO_UNREACHABLE_TASSERT(7484304);
    }

    /**
     * Clones the given vector of LetVariable objects using the newExpCtx.
     */
    void copyLetVariablesWithNewExpCtx(const std::vector<LetVariable>& src,
                                       ExpressionContext& newExpCtx);

    /**
     * Builds a parsed pipeline for introspection (e.g. constraints, dependencies). Any sub-$lookup
     * pipelines will be built recursively.
     */
    void initializeResolvedIntrospectionPipeline();

    /**
     * Validates each name in 'letVariables' and appends a corresponding entry to '_letVariables'
     * (parsed expression + variable id from '_variablesParseState').
     */
    void parseAndDefineLetVariables(const BSONObj& letVariables,
                                    const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * If '_fieldMatchPipelineIdx' is set, inserts an empty $match placeholder into
     * '_sharedState->resolvedPipeline' at that index.
     */
    void insertFieldMatchPlaceholder();

    /**
     * Given a mutable document, appends execution stats such as 'totalDocsExamined',
     * 'totalKeysExamined', 'collectionScans', 'indexesUsed', etc. to it.
     */
    void appendSpecificExecStats(MutableDocument& doc) const;

    /**
     * Returns true if we are not in a transaction.
     */
    bool foreignShardedLookupAllowed() const;

    NamespaceString _fromNs;
    NamespaceString _resolvedNs;
    bool _fromNsIsAView = false;

    // Path to the "as" field of the $lookup where the matches output array will be created.
    FieldPath _as;

    boost::optional<BSONObj> _additionalFilter;

    // For use when $lookup is specified with localField/foreignField syntax.
    boost::optional<FieldPath> _localField;
    boost::optional<FieldPath> _foreignField;
    // Indicates the index in '_sharedState->resolvedPipeline' where the local/foreignField $match
    // resides.
    boost::optional<size_t> _fieldMatchPipelineIdx;

    // Holds 'let' defined variables defined both in this stage and in parent pipelines.
    // These are copied to the '_fromExpCtx' ExpressionContext's 'variables' and
    // 'variablesParseState' for use in foreign pipeline execution.
    Variables _variables;
    VariablesParseState _variablesParseState;

    // The ExpressionContext used when performing aggregation pipelines against the '_resolvedNs'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // The aggregation pipeline defined with the user request, prior to optimization and view
    // resolution. If the user did not define a pipeline this will be 'boost::none'. Subpipelines on
    // timeseries could have no '_userPipeline' but will always create a pipeline during execution
    // to unpack raw buckets.
    boost::optional<std::vector<BSONObj>> _userPipeline;

    // Holds 'let' variables defined in $lookup stage. 'let' variables are stored in the vector in
    // order to ensure the stability in the query shape serialization.
    std::vector<LetVariable> _letVariables;

    boost::intrusive_ptr<DocumentSourceMatch> _matchSrc;
    boost::intrusive_ptr<DocumentSourceUnwind> _unwindSrc;

    std::shared_ptr<LookUpSharedState> _sharedState;
};  // class DocumentSourceLookUp

}  // namespace mongo
