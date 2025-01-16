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

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lookup_set_cache.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/spilling/spillable_deque.h"
#include "mongo/db/pipeline/spilling/spillable_map.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/memory_usage_tracker.h"

namespace mongo {

class DocumentSourceGraphLookUp final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$graphLookup"_sd;

    class LiteParsed : public LiteParsedDocumentSourceForeignCollection {
    public:
        LiteParsed(std::string parseTimeName, NamespaceString foreignNss)
            : LiteParsedDocumentSourceForeignCollection(std::move(parseTimeName),
                                                        std::move(foreignNss)) {}

        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec);


        Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                              bool inMultiDocumentTransaction) const override {
            const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
            if (!inMultiDocumentTransaction || _foreignNss != nss ||
                gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot)) {
                return Status::OK();
            }

            return Status(
                ErrorCodes::NamespaceCannotBeSharded,
                "Sharded $graphLookup is not allowed within a multi-document transaction");
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {Privilege(ResourcePattern::forExactNamespace(_foreignNss), ActionType::find)};
        }
    };

    DocumentSourceGraphLookUp(const DocumentSourceGraphLookUp&,
                              const boost::intrusive_ptr<ExpressionContext>&);

    ~DocumentSourceGraphLookUp() override;

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    const FieldPath& getConnectFromField() const {
        return _connectFromField;
    }

    const FieldPath& getConnectToField() const {
        return _connectToField;
    }

    Expression* getStartWithField() const {
        return _startWith.get();
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
    boost::intrusive_ptr<Expression>& getMutableStartWithField() {
        return _startWith;
    }

    void setStartWithField(boost::intrusive_ptr<Expression> startWith) {
        _startWith.swap(startWith);
    }

    boost::optional<BSONObj> getAdditionalFilter() const {
        return _additionalFilter;
    };

    void setAdditionalFilter(boost::optional<BSONObj> additionalFilter) {
        _additionalFilter = additionalFilter ? additionalFilter->getOwned() : additionalFilter;
    };

    void serializeToArray(std::vector<Value>& array,
                          const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * Returns the 'as' path, and possibly the fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        expression::addDependencies(_startWith.get(), deps);
        return DepsTracker::State::SEE_NEXT;
    };

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        expression::addVariableRefs(_startWith.get(), refs);
        if (_additionalFilter) {
            match_expression::addVariableRefs(
                uassertStatusOK(MatchExpressionParser::parse(*_additionalFilter, _fromExpCtx))
                    .get(),
                refs);
        }
    }
    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    bool validateOperationContext(const OperationContext* opCtx) const final;

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

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final;

    void spill(int64_t maximumMemoryUsage);

    bool usedDisk() override {
        return _visited.usedDisk() || _queue.usedDisk();
    }

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    const NamespaceString& getFromNs() const {
        return _from;
    }

protected:
    GetNextResult doGetNext() final;
    void doDispose() final;
    boost::optional<ShardId> computeMergeShardId() const final;

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwind' field.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    static constexpr StringData kFrontierValueField = "f"_sd;
    static constexpr StringData kDepthField = "d"_sd;

    DocumentSourceGraphLookUp(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        NamespaceString from,
        std::string as,
        std::string connectFromField,
        std::string connectToField,
        boost::intrusive_ptr<Expression> startWith,
        boost::optional<BSONObj> additionalFilter,
        boost::optional<FieldPath> depthField,
        boost::optional<long long> maxDepth,
        boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc);

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final {
        // Should not be called; use serializeToArray instead.
        MONGO_UNREACHABLE_TASSERT(7484306);
    }

    /**
     * Prepares the query to execute on the 'from' collection wrapped in a $match by using the
     * contents of '_queue'. Consumes from the _queue until it is empty or the match stage reached
     * BSONObjMaxUserSize.
     */
    struct Query {
        // Valid $match stage that we have to query, or boost::none if no query is needed.
        boost::optional<BSONObj> match;
        // Documents that are returned from in-memory cache.
        DocumentUnorderedSet cached;
        // Values from _queue that are processed by this query.
        ValueFlatUnorderedSet queried;
        // Depth of the documents, returned by the given query.
        long long depth;
    };
    Query makeQueryFromQueue();

    /**
     * Create pipeline to get documents from the foreign collection.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(BSONObj match,
                                                            bool allowForeignSharded);

    /**
     * If we have internalized a $unwind, getNext() dispatches to this function.
     */
    GetNextResult getNextUnwound();

    /**
     * Perform a breadth-first search of the 'from' collection. '_queue' should already be
     * populated with the values for the initial query. Populates '_visited' with the result(s)
     * of the query.
     */
    void doBreadthFirstSearch();

    /**
     * Populates '_queue' with the '_startWith' value(s) from '_input' and then performs a
     * breadth-first search. Caller should check that _input is not boost::none.
     */
    void performSearch();

    /**
     * Updates '_cache' with 'result' appropriately, given that 'result' was retrieved when querying
     * for 'queried'.
     */
    void addToCache(const Document& result, const ValueFlatUnorderedSet& queried);

    /**
     * Assert that '_visited' and '_queue' have not exceeded the maximum meory usage, and then
     * evict from '_cache' until this source is using less than 'maxMemoryUsageBytes'.
     */
    void checkMemoryUsage();

    /**
     * Wraps frontier value and depth into a Document format for the _queue.
     */
    Document wrapFrontierValue(Value value, long long depth) const;

    /**
     * Process 'result', adding it to '_visited' with the given 'depth', and updating '_queue'
     * with the object's 'connectTo' values.
     */
    void addToVisitedAndQueue(Document result, long long depth);

    /**
     * Returns true if we are not in a transaction.
     */
    bool foreignShardedGraphLookupAllowed() const;

    void updateSpillingStats();

    // $graphLookup options.
    NamespaceString _from;
    FieldPath _as;
    FieldPath _connectFromField;
    FieldPath _connectToField;
    boost::intrusive_ptr<Expression> _startWith;
    boost::optional<BSONObj> _additionalFilter;
    boost::optional<FieldPath> _depthField;
    boost::optional<long long> _maxDepth;

    // The ExpressionContext used when performing aggregation pipelines against the '_from'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // The aggregation pipeline to perform against the '_from' namespace.
    std::vector<BSONObj> _fromPipeline;

    // Tracks memory for _queue and _visited. _cache is allowed to use the remaining memory limit.
    MemoryUsageTracker _memoryUsageTracker;

    // Only used during the breadth-first search, tracks the set of values on the current frontier.
    // Contains documents with two fields: kFrontierValueField with a lookup value and kDepthField
    // with depth.
    SpillableDeque _queue;

    // Tracks nodes that have been discovered for a given input. Keys are the '_id' value of the
    // document from the foreign collection, value is the document itself.  The keys are compared
    // using the simple collation.
    SpillableDocumentMap _visited;

    // Caches query results to avoid repeating any work. This structure is maintained across calls
    // to getNext().
    LookupSetCache _cache;

    // When we have internalized a $unwind, we must keep track of the input document, since we will
    // need it for multiple "getNext()" calls.
    boost::optional<Document> _input;

    // Keep track of a $unwind that was absorbed into this stage.
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> _unwind;
    SpillableDocumentMap::Iterator _unwindIterator = _visited.end();

    // If we absorbed a $unwind that specified 'includeArrayIndex', this is used to populate that
    // field, tracking how many results we've returned so far for the current input document.
    long long _outputIndex = 0;

    // Holds variables defined both in this stage and in parent pipelines. These are copied to the
    // '_fromExpCtx' ExpressionContext's 'variables' and 'variablesParseState' for use in the
    // '_fromPipeline' execution.
    Variables _variables;
    VariablesParseState _variablesParseState;

    DocumentSourceGraphLookupStats _stats;
};

}  // namespace mongo
