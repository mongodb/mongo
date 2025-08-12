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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_lookup_gen.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/lookup_shared_state.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        LiteParsed(std::string parseTimeName,
                   NamespaceString foreignNss,
                   boost::optional<LiteParsedPipeline> pipeline)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), std::move(foreignNss), std::move(pipeline)) {}

        /**
         * Lookup from a sharded collection may not be allowed.
         */
        Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                              bool inMultiDocumentTransaction) const final {
            const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
            if (!inMultiDocumentTransaction ||
                gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot)) {
                return Status::OK();
            }
            auto involvedNss = getInvolvedNamespaces();
            if (involvedNss.find(nss) == involvedNss.end()) {
                return Status::OK();
            }

            return Status(ErrorCodes::NamespaceCannotBeSharded,
                          "Sharded $lookup is not allowed within a multi-document transaction");
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
                                           bool bypassDocumentValidation) const final;

        bool requiresAuthzChecks() const override {
            return false;
        }
    };

    /**
     * Copy constructor used for clone().
     */
    DocumentSourceLookUp(const DocumentSourceLookUp&,
                         const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void serializeToArray(std::vector<Value>& array,
                          const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * Returns the 'as' path, and possibly fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    /**
     * Reports the StageConstraints of this $lookup instance. A $lookup constructed with
     * pipeline syntax will inherit certain constraints from the stages in its pipeline.
     */
    StageConstraints constraints(PipelineSplitState) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachSourceFromOperationContext() final;

    void reattachSourceToOperationContext(OperationContext* opCtx) final;

    bool validateSourceOperationContext(const OperationContext* opCtx) const final;

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
    void setUnwindStage(const boost::intrusive_ptr<DocumentSourceUnwind>& unwind) {
        invariant(!_sharedState->unwindSrc);
        _sharedState->unwindSrc = unwind;
    }

    bool hasLocalFieldForeignFieldJoin() const {
        return _sharedState->localField != boost::none;
    }

    bool hasPipeline() const {
        return _sharedState->userPipeline != boost::none;
    }

    boost::optional<FieldPath> getForeignField() const {
        return _sharedState->foreignField;
    }

    boost::optional<FieldPath> getLocalField() const {
        return _sharedState->localField;
    }

    /**
     * "as" field must be present in all types of $lookup queries.
     */
    const FieldPath& getAsField() const {
        return _sharedState->as;
    }

    const std::vector<LetVariable>& getLetVariables() const {
        return _sharedState->letVariables;
    }

    /**
     * Returns a non-executable pipeline which can be useful for introspection. In this
     * pipeline, all view definitions are resolved. This pipeline is present in both the
     * sub-pipeline version of $lookup and the simpler 'localField/foreignField' version, but
     * because it is not tied to any document to look up it is missing variable definitions for
     * the former type and the $match stage which will be added to enforce the join criteria for
     * the latter.
     */
    const auto& getResolvedIntrospectionPipeline() const {
        return *_sharedState->resolvedIntrospectionPipeline;
    }

    auto& getResolvedIntrospectionPipeline() {
        return *_sharedState->resolvedIntrospectionPipeline;
    }
    const Variables& getVariables_forTest() {
        return _sharedState->variables;
    }

    const VariablesParseState& getVariablesParseState_forTest() {
        return _sharedState->variablesParseState;
    }
    const DocumentSourceContainer* getSubPipeline() const final {
        tassert(6080015,
                "$lookup expected to have a resolved pipeline, but didn't",
                _sharedState->resolvedIntrospectionPipeline);
        return &_sharedState->resolvedIntrospectionPipeline->getSources();
    }

    std::unique_ptr<Pipeline> getSubPipeline_forTest(const Document& inputDoc) {
        return _sharedState->buildPipeline(_sharedState->fromExpCtx, inputDoc, getExpCtx());
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final;

    SbeCompatibility sbeCompatibility() const {
        return _sbeCompatibility;
    }

    const NamespaceString& getFromNs() const {
        return _sharedState->fromNs;
    }

    const boost::intrusive_ptr<DocumentSourceUnwind>& getUnwindSource() const {
        return _sharedState->unwindSrc;
    }

    /*
     * Indicates whether this $lookup stage has absorbed an immediately following $unwind stage
     * that unwinds the lookup result array.
     */
    bool hasUnwindSrc() const {
        return bool(_sharedState->unwindSrc);
    }

    /**
     * Rebuilds the _sharedState->resolvedPipeline from the
     * _sharedState->resolvedIntrospectionPipeline. This is required for server rewrites for
     * FLE2. The server rewrite code operates on DocumentSources of a parsed pipeline, which we
     * obtain from DocumentSourceLookUp::_sharedState->resolvedIntrospectionPipeline. However,
     * we use _sharedState->resolvedPipeline to execute each iteration of doGetNext(). This
     * method is called exclusively from rewriteLookUp (server_rewrite.cpp) once the pipeline
     * has been rewritten for FLE2.
     */
    void rebuildResolvedPipeline();

    /**
     * Returns the expression context associated with foreign collection namespace and/or
     * sub-pipeline.
     */
    boost::intrusive_ptr<ExpressionContext> getSubpipelineExpCtx() {
        return _sharedState->fromExpCtx;
    }

    const std::shared_ptr<LookUpSharedState>& getSharedState() {
        return _sharedState;
    }

protected:
    boost::optional<ShardId> computeMergeShardId() const final;

    /**
     * Attempts to combine with an immediately following $unwind stage that unwinds the
     * $lookup's "as" field, setting the '_sharedState->unwindSrc' member to the absorbed
     * $unwind stage. If this is done it may also absorb one or more $match stages that
     * immediately followed the $unwind, setting the resulting combined $match in the
     * '_sharedState->matchSrc' member.
     */
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    /**
     * Target constructor. Handles common-field initialization for the syntax-specific
     * delegating constructors.
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
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final {
        MONGO_UNREACHABLE_TASSERT(7484304);
    }

    /**
     * Builds a parsed pipeline for introspection (e.g. constraints, dependencies). Any
     * sub-$lookup pipelines will be built recursively.
     */
    void initializeResolvedIntrospectionPipeline();

    /**
     * Reinitialize the cache with a new max size. May only be called if this DSLookup was
     * created with pipeline syntax only, the cache has not been frozen or abandoned, and no
     * data has been added to it.
     */
    void reInitializeCache(size_t maxCacheSizeBytes) {
        invariant(!hasLocalFieldForeignFieldJoin());
        invariant(!_sharedState->cache ||
                  (_sharedState->cache->isBuilding() && _sharedState->cache->sizeBytes() == 0));
        _sharedState->cache.emplace(maxCacheSizeBytes);
    }

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
     * Checks conditions necessary for SBE compatibility and sets '_sbeCompatibility' enum. Note:
     * when optimizing the pipeline the flag might be modified.
     */
    void determineSbeCompatibility();

    /**
     * Sets '_sbeCompatibility' enum to 'maxCompatibility' iff that *reduces* the compatibility.
     */
    inline void downgradeSbeCompatibility(SbeCompatibility maxCompatibility) {
        if (maxCompatibility < _sbeCompatibility) {
            _sbeCompatibility = maxCompatibility;
        }
    }

    NamespaceString _resolvedNs;
    bool _fromNsIsAView;

    // Can this $lookup be pushed down into SBE?
    SbeCompatibility _sbeCompatibility = SbeCompatibility::notCompatible;

    std::shared_ptr<LookUpSharedState> _sharedState;
};  // class DocumentSourceLookUp

}  // namespace mongo
