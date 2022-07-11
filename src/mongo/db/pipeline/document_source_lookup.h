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

#include <boost/optional.hpp>

#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/lookup_set_cache.h"

namespace mongo {

/**
 * Queries separate collection for equality matches with documents in the pipeline collection.
 * Adds matching documents to a new array field in the input document.
 */
class DocumentSourceLookUp final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$lookup"_sd;
    static constexpr StringData kFromField = "from"_sd;
    static constexpr StringData kLocalField = "localField"_sd;
    static constexpr StringData kForeignField = "foreignField"_sd;
    static constexpr StringData kPipelineField = "pipeline"_sd;
    static constexpr StringData kAsField = "as"_sd;

    struct LetVariable {
        LetVariable(std::string name, boost::intrusive_ptr<Expression> expression, Variables::Id id)
            : name(std::move(name)), expression(std::move(expression)), id(id) {}

        std::string name;
        boost::intrusive_ptr<Expression> expression;
        Variables::Id id;
    };

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec);

        LiteParsed(std::string parseTimeName,
                   NamespaceString foreignNss,
                   boost::optional<LiteParsedPipeline> pipeline,
                   bool hasInternalCollation)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), std::move(foreignNss), std::move(pipeline)),
              _hasInternalCollation(hasInternalCollation) {}

        /**
         * Lookup from a sharded collection may not be allowed.
         */
        bool allowShardedForeignCollection(NamespaceString nss,
                                           bool inMultiDocumentTransaction) const override final {
            if (!inMultiDocumentTransaction) {
                return true;
            }
            auto involvedNss = getInvolvedNamespaces();
            return (involvedNss.find(nss) == involvedNss.end());
        }

        void assertPermittedInAPIVersion(const APIParameters& apiParameters) const final {
            if (apiParameters.getAPIVersion() && *apiParameters.getAPIVersion() == "1" &&
                apiParameters.getAPIStrict().value_or(false)) {
                uassert(
                    ErrorCodes::APIStrictError,
                    "The _internalCollation argument to $lookup is not supported in API Version 1",
                    !_hasInternalCollation);
            }
        }

        void getForeignExecutionNamespaces(
            stdx::unordered_set<NamespaceString>& nssSet) const final {
            // We do not recurse on, nor insert '_foreignNss' in the event that this $lookup has
            // a subpipeline as such $lookup stages are not eligible for pushdown.
            if (getSubPipelines().empty()) {
                tassert(6235100, "Expected foreignNss to be initialized for $lookup", _foreignNss);
                nssSet.emplace(*_foreignNss);
            }
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override final;

    private:
        bool _hasInternalCollation = false;
    };

    /**
     * Copy constructor used for clone().
     */
    DocumentSourceLookUp(const DocumentSourceLookUp&,
                         const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final;
    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    /**
     * Returns the 'as' path, and possibly fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    /**
     * Reports the StageConstraints of this $lookup instance. A $lookup constructed with pipeline
     * syntax will inherit certain constraints from the stages in its pipeline.
     */
    StageConstraints constraints(Pipeline::SplitState) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    bool usedDisk() final;

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSource> createFromBsonWithCacheSize(
        BSONElement elem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        size_t maxCacheSizeBytes) {
        auto dsLookup = createFromBson(elem, expCtx);
        static_cast<DocumentSourceLookUp*>(dsLookup.get())->reInitializeCache(maxCacheSizeBytes);
        return dsLookup;
    }

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
    void setUnwindStage(const boost::intrusive_ptr<DocumentSourceUnwind>& unwind) {
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
    const auto& getResolvedIntrospectionPipeline() const {
        return *_resolvedIntrospectionPipeline;
    }

    auto& getResolvedIntrospectionPipeline() {
        return *_resolvedIntrospectionPipeline;
    }

    const Variables& getVariables_forTest() {
        return _variables;
    }

    const VariablesParseState& getVariablesParseState_forTest() {
        return _variablesParseState;
    }

    const Pipeline::SourceContainer* getSubPipeline() const final {
        tassert(6080015,
                "$lookup expected to have a resolved pipeline, but didn't",
                _resolvedIntrospectionPipeline);
        return &_resolvedIntrospectionPipeline->getSources();
    }

    std::unique_ptr<Pipeline, PipelineDeleter> getSubPipeline_forTest(const Document& inputDoc) {
        return buildPipeline(inputDoc);
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final;

    bool sbeCompatible() const {
        return _sbeCompatible;
    }

    const NamespaceString& getFromNs() const {
        return _fromNs;
    }

protected:
    GetNextResult doGetNext() final;
    void doDispose() final;

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwindSrc'
     * field.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    /**
     * Target constructor. Handles common-field initialization for the syntax-specific delegating
     * constructors.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx);
    /**
     * Constructor used for a $lookup stage specified using the {from: ..., localField: ...,
     * foreignField: ..., as: ...} syntax.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::string localField,
                         std::string foreignField,
                         boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
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
                         boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
                         boost::optional<std::pair<std::string, std::string>> localForeignFields,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Should not be called; use serializeToArray instead.
     */
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        MONGO_UNREACHABLE;
    }

    GetNextResult unwindResult();

    /**
     * Resolves let defined variables against 'localDoc' and stores the results in 'variables'.
     */
    void resolveLetVariables(const Document& localDoc, Variables* variables);

    /**
     * Builds a parsed pipeline for introspection (e.g. constraints, dependencies). Any sub-$lookup
     * pipelines will be built recursively.
     */
    void initializeResolvedIntrospectionPipeline();

    /**
     * Builds the $lookup pipeline using the resolved view definition for a sharded foreign view and
     * updates the '_resolvedPipeline', as well as '_fieldMatchPipelineIdx' in the case of a
     * 'foreign' join.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> buildPipelineFromViewDefinition(
        std::vector<BSONObj> serializedPipeline,
        ExpressionContext::ResolvedNamespace resolvedNamespace);

    /**
     * Builds the $lookup pipeline and resolves any variables using the passed 'inputDoc', adding a
     * cursor and/or cache source as appropriate.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> buildPipeline(const Document& inputDoc);

    /**
     * Reinitialize the cache with a new max size. May only be called if this DSLookup was created
     * with pipeline syntax only, the cache has not been frozen or abandoned, and no data has been
     * added to it.
     */
    void reInitializeCache(size_t maxCacheSizeBytes) {
        invariant(!hasLocalFieldForeignFieldJoin());
        invariant(!_cache || (_cache->isBuilding() && _cache->sizeBytes() == 0));
        _cache.emplace(maxCacheSizeBytes);
    }

    /**
     * Method to add a DocumentSourceSequentialDocumentCache stage and optimize the pipeline to
     * move the cache to its final position.
     */
    void addCacheStageAndOptimize(Pipeline& pipeline);

    /**
     * Given a mutable document, appends execution stats such as 'totalDocsExamined',
     * 'totalKeysExamined', 'collectionScans', 'indexesUsed', etc. to it.
     */
    void appendSpecificExecStats(MutableDocument& doc) const;

    /**
     * Returns true if we are not in a transaction.
     */
    bool foreignShardedLookupAllowed() const;

    /**
     * Checks conditions necessary for SBE compatibility and sets _sbeCompatible flag. Note: when
     * optimizing the pipeline the flag might be modified.
     */
    void determineSbeCompatibility();

    DocumentSourceLookupStats _stats;

    NamespaceString _fromNs;
    NamespaceString _resolvedNs;
    FieldPath _as;
    boost::optional<BSONObj> _additionalFilter;

    // For use when $lookup is specified with localField/foreignField syntax.
    boost::optional<FieldPath> _localField;
    boost::optional<FieldPath> _foreignField;
    // Indicates the index in '_resolvedPipeline' where the local/foreignField $match resides.
    boost::optional<size_t> _fieldMatchPipelineIdx;

    // Holds 'let' defined variables defined both in this stage and in parent pipelines. These are
    // copied to the '_fromExpCtx' ExpressionContext's 'variables' and 'variablesParseState' for use
    // in foreign pipeline execution.
    Variables _variables;
    VariablesParseState _variablesParseState;

    // Caches documents returned by the non-correlated prefix of the $lookup pipeline during the
    // first iteration, up to a specified size limit in bytes. If this limit is not exceeded by the
    // time we hit EOF, subsequent iterations of the pipeline will draw from the cache rather than
    // from a cursor source.
    boost::optional<SequentialDocumentCache> _cache;

    // The ExpressionContext used when performing aggregation pipelines against the '_resolvedNs'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // When a `_internalCollation` has been specified on a $lookup stage, we will set that collation
    // on `_fromExpCtx`. An explicit simple collation however is represented in the same way as the
    // default binary collation. We need to differentiate between the two to avoid serializing the
    // collation when not set explicitly.
    bool _hasExplicitCollation = false;

    // Can this $lookup be pushed down into SBE?
    bool _sbeCompatible = false;

    // The aggregation pipeline to perform against the '_resolvedNs' namespace. Referenced view
    // namespaces have been resolved.
    std::vector<BSONObj> _resolvedPipeline;
    // The aggregation pipeline defined with the user request, prior to optimization and view
    // resolution. If the user did not define a pipeline this will be 'boost::none'.
    boost::optional<std::vector<BSONObj>> _userPipeline;
    // A pipeline parsed from _resolvedPipeline at creation time, intended to support introspective
    // functions. If sub-$lookup stages are present, their pipelines are constructed recursively.
    std::unique_ptr<Pipeline, PipelineDeleter> _resolvedIntrospectionPipeline;

    std::vector<LetVariable> _letVariables;

    boost::intrusive_ptr<DocumentSourceMatch> _matchSrc;
    boost::intrusive_ptr<DocumentSourceUnwind> _unwindSrc;

    // The following members are used to hold onto state across getNext() calls when '_unwindSrc' is
    // not null.
    long long _cursorIndex = 0;
    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
    boost::optional<Document> _input;
    boost::optional<Document> _nextValue;
};

}  // namespace mongo
