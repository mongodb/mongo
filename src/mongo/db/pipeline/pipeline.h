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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class BSONObj;
class OperationContext;
class Pipeline;

/**
 * Enabling the disablePipelineOptimization fail point will stop the aggregate command from
 * attempting to optimize the pipeline or the pipeline stages. Neither DocumentSource::optimizeAt()
 * nor DocumentSource::optimize() will be attempted.
 */
extern FailPoint disablePipelineOptimization;

using PipelineValidatorCallback = std::function<void(const Pipeline&)>;

struct MakePipelineOptions {
    bool optimize = true;

    // It is assumed that the pipeline has already been optimized when we create the
    // MakePipelineOptions. If this is not the case, the caller is responsible for setting
    // alreadyOptimized to false.
    bool alreadyOptimized = true;
    bool attachCursorSource = true;

    // When set to true, ensures that default collection collator will be attached to the pipeline.
    // Needs 'attachCursorSource' set to true, in order to be applied.
    bool useCollectionDefaultCollator = false;
    ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed;
    PipelineValidatorCallback validator = nullptr;
    boost::optional<BSONObj> readConcern;
};

/**
 * A Pipeline object represents a list of DocumentSources and is responsible for optimizing the
 * pipeline.
 */
class Pipeline {
public:
    /**
     * The list of default supported match expression features.
     */
    static constexpr MatchExpressionParser::AllowedFeatureSet kAllowedMatcherFeatures =
        MatchExpressionParser::AllowedFeatures::kText |
        MatchExpressionParser::AllowedFeatures::kExpr |
        MatchExpressionParser::AllowedFeatures::kJSONSchema |
        MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    /**
     * The match expression features allowed when running a pipeline with $geoNear.
     */
    static constexpr MatchExpressionParser::AllowedFeatureSet kGeoNearMatcherFeatures =
        MatchExpressionParser::AllowedFeatures::kText |
        MatchExpressionParser::AllowedFeatures::kExpr |
        MatchExpressionParser::AllowedFeatures::kJSONSchema |
        MatchExpressionParser::AllowedFeatures::kEncryptKeywords |
        MatchExpressionParser::AllowedFeatures::kGeoNear;

    /**
     * Parses a Pipeline from a vector of BSONObjs then invokes the optional 'validator' callback
     * with a reference to the newly created Pipeline. If no validator callback is given, this
     * method assumes that we're parsing a top-level pipeline. Throws an exception if it failed to
     * parse or if any exception occurs in the validator. The returned pipeline is not optimized,
     * but the caller may convert it to an optimized pipeline by calling optimizePipeline().
     *
     * It is illegal to create a pipeline using an ExpressionContext which contains a collation that
     * will not be used during execution of the pipeline. Doing so may cause comparisons made during
     * parse-time to return the wrong results.
     */
    static std::unique_ptr<Pipeline> parse(const std::vector<BSONObj>& rawPipeline,
                                           const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           PipelineValidatorCallback validator = nullptr);

    /**
     * Parses sub-pipelines from a $facet aggregation. Like parse(), but skips top-level
     * validators.
     */
    static std::unique_ptr<Pipeline> parseFacetPipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        PipelineValidatorCallback validator = nullptr);

    /**
     * Like parse, but takes a BSONElement instead of a vector of objects. 'arrElem' must be an
     * array of objects.
     */
    static std::unique_ptr<Pipeline> parseFromArray(
        BSONElement arrayElem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        PipelineValidatorCallback validator = nullptr);

    /**
     * Creates a Pipeline from an existing DocumentSourceContainer.
     *
     * Returns a non-OK status if any stage is in an invalid position. For example, if an $out stage
     * is present but is not the last stage.
     */
    static std::unique_ptr<Pipeline> create(DocumentSourceContainer sources,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Returns true if the provided aggregation command has an $out or $merge stage.
     */
    static bool aggHasWriteStage(const BSONObj& cmd);

    /**
     * Parses a Pipeline from a vector of BSONObjs representing DocumentSources. The state of the
     * returned pipeline will depend upon the supplied MakePipelineOptions:
     * - The boolean opts.optimize determines whether the pipeline will be optimized.
     * - If opts.attachCursorSource is false, the pipeline will be returned without attempting to
     * add an initial cursor source.
     */
    static std::unique_ptr<Pipeline> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        MakePipelineOptions opts = MakePipelineOptions{});

    /**
     * Creates a Pipeline from an AggregateCommandRequest. This preserves any aggregation options
     * set on the aggRequest. The state of the returned pipeline will depend upon the supplied
     * MakePipelineOptions:
     * - The boolean opts.optimize determines whether the pipeline will be optimized.
     * - If opts.attachCursorSource is false, the pipeline will be returned without attempting to
     * add an initial cursor source.
     *
     * This function throws if parsing the pipeline set on aggRequest failed.
     */
    static std::unique_ptr<Pipeline> makePipeline(
        AggregateCommandRequest& aggRequest,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        MakePipelineOptions opts = MakePipelineOptions{});

    /**
     * Optimize the given pipeline after the stage that 'itr' points to.
     *
     * Returns a valid iterator that points to the new "end of the pipeline": i.e., the stage that
     * comes after 'itr' in the newly optimized pipeline.
     */
    static DocumentSourceContainer::iterator optimizeEndOfPipeline(
        DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);

    static std::unique_ptr<Pipeline> viewPipelineHelperForSearch(
        const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
        ResolvedNamespace resolvedNs,
        std::vector<BSONObj> currentPipeline,
        MakePipelineOptions opts,
        NamespaceString originalNs);

    static std::unique_ptr<Pipeline> makePipelineFromViewDefinition(
        const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
        ResolvedNamespace resolvedNs,
        std::vector<BSONObj> currentPipeline,
        MakePipelineOptions opts,
        NamespaceString originalNs);

    /**
     * Callers can optionally specify 'newExpCtx' to construct the deep clone with it. This will be
     * used to construct all the cloned DocumentSources as well.
     *
     * The the resulting pipeline will have default values for '_splitStage', '_disposed',
     * '_isParameterized', and 'frozen' properties.
     */
    std::unique_ptr<Pipeline> clone(const boost::intrusive_ptr<ExpressionContext>& = nullptr) const;

    const boost::intrusive_ptr<ExpressionContext>& getContext() const {
        return pCtx;
    }


    bool isFrozen() const {
        return _frozen;
    }

    /**
     * Communicates to the pipeline which part of a split pipeline it is when the pipeline has been
     * split in two.
     */
    void setSplitState(PipelineSplitState state) {
        _splitState = state;
    }

    const Pipeline& freeze() {
        _frozen = true;
        return *this;
    }

    /**
     * If the pipeline starts with a stage which is or includes a query predicate (e.g. a $match),
     * returns a BSON object representing that query. Otherwise, returns an empty BSON object.
     */
    BSONObj getInitialQuery() const;

    /**
     * Convenience wrapper that parameterizes a pipeline's match stage, if present.
     */
    void parameterize();

    /**
     * Clear any parameterization in the pipeline.
     */
    void unparameterize();

    /**
     * Returns 'true' if a pipeline's structure is eligible for parameterization. It must have a
     * $match first stage.
     */
    bool canParameterize() const;

    /**
     * Returns 'true' if a pipeline is parameterized.
     */
    bool isParameterized() const {
        return _isParameterized;
    }

    /**
     * Returns a specific ShardId that should be merger for this pipeline or boost::none if it is
     * not needed.
     */
    boost::optional<ShardId> needsSpecificShardMerger() const;

    /**
     * Returns 'true' if the pipeline must merge on router.
     */
    bool needsRouterMerger() const;

    /**
     * Returns 'true' if any stage in the pipeline must run on a shard.
     */
    bool needsShard() const;

    /**
     * Returns 'true' if any stage in the pipeline requires being run on all hosts within targeted
     * shards.
     */
    bool needsAllShardHosts() const;

    /**
     * Returns Status::OK() if the pipeline can run on router, but is not obliged to; that is, it
     * can run either on mongoS or on a shard.
     */
    Status canRunOnRouter() const;

    /**
     * Returns true if this pipeline must only run on router. Can be called on unsplit or merge
     * pipelines, but not on the shards part of a split pipeline.
     */
    bool requiredToRunOnRouter() const;

    /**
     * Checks whether the pipeline can run on the specified collection using catalog data. It is the
     * caller's responsibility to ensure the catalog data is accurate.
     */
    void validateWithCollectionMetadata(const CollectionOrViewAcquisition& collOrView) const;
    void validateWithCollectionMetadata(const CollectionRoutingInfo& cri) const;

    /**
     * Modifies the pipeline in-place to perform any rewrites that must happen before optimization.
     */
    void performPreOptimizationRewrites(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        const CollectionRoutingInfo& cri);
    void performPreOptimizationRewrites(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        const CollectionOrViewAcquisition& collOrView);

    /**
     * Modifies the pipeline, optimizing it by combining and swapping stages.
     */
    void optimizePipeline();

    /**
     * Modifies the container, optimizes each stage individually.
     */
    static void optimizeEachStage(DocumentSourceContainer* container);

    /**
     * Modifies the container, optimizing it by combining, swapping, dropping and/or inserting
     * stages.
     */
    static void optimizeContainer(DocumentSourceContainer* container);

    /**
     * Returns any other collections involved in the pipeline in addition to the collection the
     * aggregation is run on. All namespaces returned are the names of collections, after views have
     * been resolved.
     */
    stdx::unordered_set<NamespaceString> getInvolvedCollections() const;

    /**
     * Helpers to serialize a pipeline. If serializing for logging, use one of the "*forLogging"
     * helpers, which handle redaction.
     */
    std::vector<Value> serialize(
        boost::optional<const SerializationOptions&> opts = boost::none) const;
    std::vector<BSONObj> serializeToBson(
        boost::optional<const SerializationOptions&> opts = boost::none) const;
    static std::vector<Value> serializeContainer(
        const DocumentSourceContainer& container,
        boost::optional<const SerializationOptions&> opts = boost::none);

    std::vector<BSONObj> serializeForLogging(
        boost::optional<const SerializationOptions&> opts = boost::none) const;
    static std::vector<BSONObj> serializeContainerForLogging(
        const DocumentSourceContainer& container,
        boost::optional<const SerializationOptions&> opts = boost::none);
    static std::vector<BSONObj> serializePipelineForLogging(const std::vector<BSONObj>& pipeline);

    // The initial source is special since it varies between mongos and mongod.
    void addInitialSource(boost::intrusive_ptr<DocumentSource> source);

    void addFinalSource(boost::intrusive_ptr<DocumentSource> source);

    void addSourceAtPosition(boost::intrusive_ptr<DocumentSource> source, size_t index);

    /**
     * Write the pipeline's operators to a std::vector<Value>, providing the level of detail
     * specified by 'verbosity'.
     */
    std::vector<Value> writeExplainOps(
        const SerializationOptions& opts = SerializationOptions{}) const;

    /**
     * Returns the dependencies needed by this pipeline. 'availableMetadata' should reflect what
     * metadata is present on documents that are input to the front of the pipeline. If
     * 'availableMetadata' is specified, this method will throw if any of the dependencies
     * reference unavailable metadata.
     */
    DepsTracker getDependencies(DepsTracker::MetadataDependencyValidation availableMetadata) const;

    /**
     * Populate 'refs' with the variables referred to by this pipeline, including user and system
     * variables but excluding $$ROOT. Note that field path references are not considered variables.
     */
    void addVariableRefs(std::set<Variables::Id>* refs) const;

    /**
     * Returns the dependencies needed by the DocumentSourceContainer. 'availableMetadata' should
     * reflect what metadata is present on documents that are input to the front of the pipeline. If
     * 'availableMetadata' is specified, this method will throw if any of the dependencies
     * reference unavailable metadata.
     */
    static DepsTracker getDependenciesForContainer(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceContainer& container,
        DepsTracker::MetadataDependencyValidation availableMetadata);

    /**
     * Validates metadata field dependencies in the pipeline and throws user errors if there are any
     * invalid references. For example, if the pipeline refers to {$meta: "geoNearDistance"} but
     * there is no $geoNear stage to generate that metadata, this will throw an error.
     * 'availableMetadata' should reflect what metadata is present on documents that are input to
     * the front of the pipeline.
     *
     * TODO SERVER-40900 This function is currently best-effort and does not guarantee to detect all
     * such errors.
     */
    void validateMetaDependencies(
        QueryMetadataBitSet availableMetadata = DepsTracker::kNoMetadata) const;


    /**
     * Returns a boolean that indicates whether or not a pipeline generates the provided metadata
     * 'type'.
     *
     * WARNING: Calling this function will also validate that the metadata dependencies in the
     * pipeline are valid, since tracking "available metadata" is currently tied to validating
     * dependencies as well. This function could throw a uassert if there are invalid $meta
     * references to unavailable metadata fields.
     * TODO SERVER-100902: Consider separating these concerns so that this function can be called
     * without risk of throwing a uassert.
     */
    bool generatesMetadataType(DocumentMetadataFields::MetaType type) const;

    const DocumentSourceContainer& getSources() const {
        return _sources;
    }

    DocumentSourceContainer& getSources() {
        return _sources;
    }

    MONGO_COMPILER_ALWAYS_INLINE DocumentSourceContainer::size_type size() const {
        return _sources.size();
    }

    MONGO_COMPILER_ALWAYS_INLINE bool empty() const {
        return _sources.empty();
    }

    /**
     * Removes and returns the first stage of the pipeline. Returns nullptr if the pipeline is
     * empty.
     */
    boost::intrusive_ptr<DocumentSource> popFront();

    /**
     * Returns a pointer to the first stage of the pipeline, or a nullptr if the pipeline is empty.
     */
    DocumentSource* peekFront() const;

    /**
     * Removes and returns the last stage of the pipeline. Returns nullptr if the pipeline is empty.
     */
    boost::intrusive_ptr<DocumentSource> popBack();

    /**
     * Adds the given stage to the end of the pipeline.
     */
    void pushBack(boost::intrusive_ptr<DocumentSource>);

    /**
     * Removes and returns the first stage of the pipeline if its name is 'targetStageName'.
     * Returns nullptr if there is no first stage with that name.
     */
    boost::intrusive_ptr<DocumentSource> popFrontWithName(StringData targetStageName);

    /**
     * Removes and returns the first stage of the pipeline if its name is 'targetStageName' and the
     * given 'predicate' function, if present, returns 'true' when called with a pointer to the
     * stage. Returns nullptr if there is no first stage which meets these criteria.
     */
    boost::intrusive_ptr<DocumentSource> popFrontWithNameAndCriteria(
        StringData targetStageName, std::function<bool(const DocumentSource* const)> predicate);

    /**
     * Appends another pipeline to the existing pipeline.
     * NOTE: The other pipeline will be destroyed.
     */
    void appendPipeline(std::unique_ptr<Pipeline> otherPipeline);

    /**
     * Performs common validation for top-level or facet pipelines. Throws if the pipeline is
     * invalid.
     *
     * Includes checking for illegal stage positioning. For example, $out must be at the end, while
     * a $match stage with a text query must be at the start. Note that this method accepts an
     * initial source as the first stage, which is illegal for $facet pipelines.
     */
    void validateCommon(bool alreadyOptimized) const;

    /**
     * PipelineD is a "sister" class that has additional functionality for the Pipeline. It exists
     * because of linkage requirements. Pipeline needs to function in mongod and mongos. PipelineD
     * contains extra functionality required in mongod, and which can't appear in mongos because the
     * required symbols are unavailable for linking there. Consider PipelineD to be an extension of
     * this class for mongod only.
     */
    friend class PipelineD;

    /**
     * For commands that return multiple pipelines, this value will contain the type of pipeline.
     * This can be populated to the cursor so consumers do not have to depend on order or guess
     * which pipeline is which. Default to a regular result pipeline.
     */
    CursorTypeEnum pipelineType = CursorTypeEnum::DocumentResult;

    /**
     * Get a string representation of the pipeline type.
     */
    auto getTypeString() {
        return CursorType_serializer(pipelineType);
    }

    /**
     * Sets the OperationContext of 'expCtx' to nullptr and calls 'detachFromOperationContext()' on
     * all underlying DocumentSources.
     */
    void detachFromOperationContext();

    /**
     * Sets the OperationContext of 'expCtx' to 'opCtx', and reattaches all underlying
     * DocumentSources to 'opCtx'.
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Recursively validate the operation contexts associated with this pipeline. Return true if
     * all document sources and subpipelines point to the given operation context.
     */
    bool validateOperationContext(const OperationContext* opCtx) const;

    /**
     * Asserts whether operation contexts associated with this pipeline are consistent across
     * sources.
     */
    void checkValidOperationContext() const;

private:
    Pipeline(const boost::intrusive_ptr<ExpressionContext>& pCtx);
    Pipeline(DocumentSourceContainer stages, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    /**
     * Helper for public methods that parse pipelines from vectors of different types.
     */
    template <class T>
    static std::unique_ptr<Pipeline> parseCommon(
        const std::vector<T>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        PipelineValidatorCallback validator,
        bool isFacetPipeline,
        std::function<BSONObj(T)> getElemFunc);


    DocumentSourceContainer _sources;

    PipelineSplitState _splitState = PipelineSplitState::kUnsplit;
    boost::intrusive_ptr<ExpressionContext> pCtx;
    bool _isParameterized = false;

    // Do not allow modifications of this pipeline.
    bool _frozen{false};
};

using PipelinePtr = std::unique_ptr<Pipeline>;
}  // namespace mongo
