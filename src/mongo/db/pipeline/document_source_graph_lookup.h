// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_graph_lookup.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

struct GraphLookUpParams {
    NamespaceString from;
    FieldPath as;
    FieldPath connectFromField;
    FieldPath connectToField;
    boost::intrusive_ptr<Expression> startWith;
    boost::optional<BSONObj> additionalFilter;
    boost::optional<FieldPath> depthField;
    boost::optional<long long> maxDepth;
    boost::optional<OwnedLiteParsedPipeline>
        fromLpp;  // set in constructor; nullopt for regular collections
};

class DocumentSourceGraphLookUp final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$graphLookup"sv;

    DocumentSourceGraphLookUp(const DocumentSourceGraphLookUp&,
                              const boost::intrusive_ptr<ExpressionContext>&);

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    inline const FieldPath& getConnectFromField() const {
        return _params.connectFromField;
    }

    inline const FieldPath& getConnectToField() const {
        return _params.connectToField;
    }

    inline Expression* getStartWithField() const {
        return _params.startWith.get();
    }

    const boost::intrusive_ptr<DocumentSourceUnwind>& getUnwindSource() const {
        return *_unwind;
    }

    /*
     * Indicates whether this $graphLookup stage has absorbed an immediately following $unwind stage
     * that unwinds the lookup result array.
     */
    bool hasUnwindSource() const {
        return _unwind.has_value();
    }

    /*
     * Returns a ref to '_startWith' that can be swapped out with a new expression.
     */
    inline boost::intrusive_ptr<Expression>& getMutableStartWithField() {
        return _params.startWith;
    }

    void setStartWithField(boost::intrusive_ptr<Expression> startWith) {
        _params.startWith.swap(startWith);
    }

    inline boost::optional<BSONObj> getAdditionalFilter() const {
        return _params.additionalFilter;
    };

    void setAdditionalFilter(boost::optional<BSONObj> additionalFilter) {
        _params.additionalFilter =
            additionalFilter ? additionalFilter->getOwned() : additionalFilter;
    };

    void serializeToArray(std::vector<Value>& array,
                          const query_shape::SerializationOptions& opts =
                              query_shape::SerializationOptions{}) const final;

    /**
     * Returns the 'as' path, and possibly the fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        expression::addDependencies(_params.startWith.get(), deps);
        return DepsTracker::State::SEE_NEXT;
    };

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        expression::addVariableRefs(_params.startWith.get(), refs);
        if (_params.additionalFilter) {
            dependency_analysis::addVariableRefs(
                uassertStatusOK(
                    MatchExpressionParser::parse(*_params.additionalFilter, _fromExpCtx))
                    .get(),
                refs);
        }
    }
    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachSourceFromOperationContext() final;

    void reattachSourceToOperationContext(OperationContext* opCtx) final;

    bool validateSourceOperationContext(const OperationContext* opCtx) const final;

    static boost::intrusive_ptr<DocumentSourceGraphLookUp> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        NamespaceString fromNs,
        std::string asField,
        std::string connectFromField,
        std::string connectToField,
        boost::intrusive_ptr<Expression> startWith,
        boost::optional<BSONObj> additionalFilter,
        boost::optional<FieldPath> depthField,
        boost::optional<long long> maxDepth,
        boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc);

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSource> createFromStageParams(
        GraphLookUpStageParams& params, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final;

    inline const NamespaceString& getFromNs() const {
        return _params.from;
    }

    inline const FieldPath& getAsField() const {
        return _params.as;
    }

    inline const boost::optional<FieldPath>& getDepthField() const {
        return _params.depthField;
    }

    inline const boost::optional<long long>& getMaxDepth() const {
        return _params.maxDepth;
    }

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwind' field.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

protected:
    boost::optional<ShardId> computeMergeShardId() const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceGraphLookUpToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceGraphLookUp(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        GraphLookUpParams params,
        boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc);

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final {
        // Should not be called; use serializeToArray instead.
        MONGO_UNREACHABLE_TASSERT(7484306);
    }

    /**
     * Returns true if we are not in a transaction.
     */
    bool foreignShardedGraphLookupAllowed() const;

    GraphLookUpParams _params;

    // The ExpressionContext used when performing aggregation pipelines against the '_from'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // Keep track of a $unwind that was absorbed into this stage.
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> _unwind;

    // Holds variables defined both in this stage and in parent pipelines. These are copied to the
    // '_fromExpCtx' ExpressionContext's 'variables' and 'variablesParseState' for use in the
    // '_params.fromLpp' execution.
    Variables _variables;
    VariablesParseState _variablesParseState;
};

}  // namespace mongo
