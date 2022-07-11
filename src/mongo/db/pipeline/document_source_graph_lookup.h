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

#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lookup_set_cache.h"

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


        bool allowShardedForeignCollection(NamespaceString nss,
                                           bool inMultiDocumentTransaction) const override {
            return !inMultiDocumentTransaction || _foreignNss != nss;
        }

        PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const {
            return {Privilege(ResourcePattern::forExactNamespace(_foreignNss), ActionType::find)};
        }
    };

    DocumentSourceGraphLookUp(const DocumentSourceGraphLookUp&,
                              const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final;

    const FieldPath& getConnectFromField() const {
        return _connectFromField;
    }

    const FieldPath& getConnectToField() const {
        return _connectToField;
    }

    Expression* getStartWithField() const {
        return _startWith.get();
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

    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    /**
     * Returns the 'as' path, and possibly the fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        _startWith->addDependencies(deps);
        return DepsTracker::State::SEE_NEXT;
    };

    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

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

protected:
    GetNextResult doGetNext() final;
    void doDispose() final;

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwind' field.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
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

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        // Should not be called; use serializeToArray instead.
        MONGO_UNREACHABLE;
    }

    /**
     * Prepares the query to execute on the 'from' collection wrapped in a $match by using the
     * contents of '_frontier'.
     *
     * Fills 'cached' with any values that were retrieved from the cache.
     *
     * Returns boost::none if no query is necessary, i.e., all values were retrieved from the cache.
     * Otherwise, returns a query object.
     */
    boost::optional<BSONObj> makeMatchStageFromFrontier(DocumentUnorderedSet* cached);

    /**
     * If we have internalized a $unwind, getNext() dispatches to this function.
     */
    GetNextResult getNextUnwound();

    /**
     * Perform a breadth-first search of the 'from' collection. '_frontier' should already be
     * populated with the values for the initial query. Populates '_discovered' with the result(s)
     * of the query.
     */
    void doBreadthFirstSearch();

    /**
     * Populates '_frontier' with the '_startWith' value(s) from '_input' and then performs a
     * breadth-first search. Caller should check that _input is not boost::none.
     */
    void performSearch();

    /**
     * Updates '_cache' with 'result' appropriately, given that 'result' was retrieved when querying
     * for 'queried'.
     */
    void addToCache(const Document& result, const ValueUnorderedSet& queried);

    /**
     * Assert that '_visited' and '_frontier' have not exceeded the maximum meory usage, and then
     * evict from '_cache' until this source is using less than '_maxMemoryUsageBytes'.
     */
    void checkMemoryUsage();

    /**
     * Process 'result', adding it to '_visited' with the given 'depth', and updating '_frontier'
     * with the object's 'connectTo' values.
     *
     * Returns whether '_visited' was updated, and thus, whether the search should recurse.
     */
    bool addToVisitedAndFrontier(Document result, long long depth);

    /**
     * Returns true if we are not in a transaction.
     */
    bool foreignShardedGraphLookupAllowed() const;

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

    size_t _maxMemoryUsageBytes = 100 * 1024 * 1024;

    // Track memory usage to ensure we don't exceed '_maxMemoryUsageBytes'.
    size_t _visitedUsageBytes = 0;
    size_t _frontierUsageBytes = 0;

    // Only used during the breadth-first search, tracks the set of values on the current frontier.
    ValueUnorderedSet _frontier;

    // Tracks nodes that have been discovered for a given input. Keys are the '_id' value of the
    // document from the foreign collection, value is the document itself.  The keys are compared
    // using the simple collation.
    ValueUnorderedMap<Document> _visited;

    // Caches query results to avoid repeating any work. This structure is maintained across calls
    // to getNext().
    LookupSetCache _cache;

    // When we have internalized a $unwind, we must keep track of the input document, since we will
    // need it for multiple "getNext()" calls.
    boost::optional<Document> _input;

    // Keep track of a $unwind that was absorbed into this stage.
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> _unwind;

    // If we absorbed a $unwind that specified 'includeArrayIndex', this is used to populate that
    // field, tracking how many results we've returned so far for the current input document.
    long long _outputIndex = 0;

    // Holds variables defined both in this stage and in parent pipelines. These are copied to the
    // '_fromExpCtx' ExpressionContext's 'variables' and 'variablesParseState' for use in the
    // '_fromPipeline' execution.
    Variables _variables;
    VariablesParseState _variablesParseState;
};

}  // namespace mongo
