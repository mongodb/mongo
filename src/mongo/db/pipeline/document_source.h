/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <deque>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/connpool.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/db/pipeline/lookup_set_cache.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

class Document;
class Expression;
class ExpressionFieldPath;
class ExpressionObject;
class DocumentSourceLimit;
class DocumentSourceSort;
class PlanExecutor;
class RecordCursor;

/**
 * Registers a DocumentSource to have the name 'key'. When a stage with name '$key' is found,
 * 'parser' will be called to construct a DocumentSource.
 *
 * This can also be used for stages like $project and $addFields which share common functionality
 * in the unregistered DocumentSourceSingleDocumentTransformation, or for any future single-stage
 * aliases.
 *
 * As an example, if your document source looks like {"$foo": <args>}, with a parsing function
 * 'createFromBson', you would add this line:
 * REGISTER_DOCUMENT_SOURCE(foo, DocumentSourceFoo::createFromBson);
 */
#define REGISTER_DOCUMENT_SOURCE(key, parser)                                                      \
    MONGO_INITIALIZER(addToDocSourceParserMap_##key)(InitializerContext*) {                        \
        auto parserWrapper = [](BSONElement stageSpec,                                             \
                                const boost::intrusive_ptr<ExpressionContext>& expCtx) {           \
            return std::vector<boost::intrusive_ptr<DocumentSource>>{(parser)(stageSpec, expCtx)}; \
        };                                                                                         \
        DocumentSource::registerParser("$" #key, parserWrapper);                                   \
        return Status::OK();                                                                       \
    }

/**
 * Registers a multi-stage alias to have the single name 'key'. When a stage with name '$key' is
 * found, 'parser' will be called to construct a vector of DocumentSources.
 *
 * As an example, if your document source looks like {"$foo": <args>}, with a parsing function
 * 'createFromBson', you would add this line:
 * REGISTER_MULTI_STAGE_ALIAS(foo, DocumentSourceFoo::createFromBson);
 */
#define REGISTER_MULTI_STAGE_ALIAS(key, parser)                                  \
    MONGO_INITIALIZER(addAliasToDocSourceParserMap_##key)(InitializerContext*) { \
        DocumentSource::registerParser("$" #key, (parser));                      \
        return Status::OK();                                                     \
    }

class DocumentSource : public IntrusiveCounterUnsigned {
public:
    using Parser = stdx::function<std::vector<boost::intrusive_ptr<DocumentSource>>(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&)>;

    /**
     * This is what is returned from the main DocumentSource API: getNext(). It is essentially a
     * (ReturnStatus, Document) pair, with the first entry being used to communicate information
     * about the execution of the DocumentSource, such as whether or not it has been exhausted.
     */
    class GetNextResult {
    public:
        enum class ReturnStatus {
            // There is a result to be processed.
            kAdvanced,
            // There will be no further results.
            kEOF,
            // There is not a result to be processed yet, but there may be more results in the
            // future. If a DocumentSource retrieves this status from its child, it must propagate
            // it without doing any further work.
            kPauseExecution,
        };

        static GetNextResult makeEOF() {
            return GetNextResult(ReturnStatus::kEOF);
        }

        static GetNextResult makePauseExecution() {
            return GetNextResult(ReturnStatus::kPauseExecution);
        }

        /**
         * Shortcut constructor for the common case of creating an 'advanced' GetNextResult from the
         * given 'result'. Accepts only an rvalue reference as an argument, since DocumentSources
         * will want to move 'result' into this GetNextResult, and should have to opt in to making a
         * copy.
         */
        /* implicit */ GetNextResult(Document&& result)
            : _status(ReturnStatus::kAdvanced), _result(std::move(result)) {}

        /**
         * Gets the result document. It is an error to call this if isAdvanced() returns false.
         */
        const Document& getDocument() const {
            dassert(isAdvanced());
            return _result;
        }

        /**
         * Releases the result document, transferring ownership to the caller. It is an error to
         * call this if isAdvanced() returns false.
         */
        Document releaseDocument() {
            dassert(isAdvanced());
            return std::move(_result);
        }

        ReturnStatus getStatus() const {
            return _status;
        }

        bool isAdvanced() const {
            return _status == ReturnStatus::kAdvanced;
        }

        bool isEOF() const {
            return _status == ReturnStatus::kEOF;
        }

        bool isPaused() const {
            return _status == ReturnStatus::kPauseExecution;
        }

    private:
        GetNextResult(ReturnStatus status) : _status(status) {}

        ReturnStatus _status;
        Document _result;
    };

    virtual ~DocumentSource() {}

    /**
     * The main execution API of a DocumentSource. Returns an intermediate query result generated by
     * this DocumentSource.
     *
     * All implementers must call pExpCtx->checkForInterrupt().
     */
    virtual GetNextResult getNext() = 0;

    /**
     * Inform the source that it is no longer needed and may release its resources.  After
     * dispose() is called the source must still be able to handle iteration requests, but may
     * become eof().
     * NOTE: For proper mutex yielding, dispose() must be called on any DocumentSource that will
     * not be advanced until eof(), see SERVER-6123.
     */
    virtual void dispose();

    /**
       Get the source's name.

       @returns the std::string name of the source as a constant string;
         this is static, and there's no need to worry about adopting it
     */
    virtual const char* getSourceName() const;

    /**
      Set the underlying source this source should use to get Documents
      from.

      It is an error to set the source more than once.  This is to
      prevent changing sources once the original source has been started;
      this could break the state maintained by the DocumentSource.

      This pointer is not reference counted because that has led to
      some circular references.  As a result, this doesn't keep
      sources alive, and is only intended to be used temporarily for
      the lifetime of a Pipeline::run().

      @param pSource the underlying source to use
     */
    virtual void setSource(DocumentSource* pSource);

    /**
     * Gets a BSONObjSet representing the sort order(s) of the output of the stage.
     */
    virtual BSONObjSet getOutputSorts() {
        return SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    /**
     * Returns an optimized DocumentSource that is semantically equivalent to this one, or
     * nullptr if this stage is a no-op. Implementations are allowed to modify themselves
     * in-place and return a pointer to themselves. For best results, first optimize the pipeline
     * with the optimizePipeline() method defined in pipeline.cpp.
     *
     * This is intended for any operations that include expressions, and provides a hook for
     * those to optimize those operations.
     *
     * The default implementation is to do nothing and return yourself.
     */
    virtual boost::intrusive_ptr<DocumentSource> optimize();

    /**
     * Attempt to perform an optimization with the following source in the pipeline. 'container'
     * refers to the entire pipeline, and 'itr' points to this stage within the pipeline.
     *
     * The return value is an iterator over the same container which points to the first location
     * in the container at which an optimization may be possible.
     *
     * For example, if a swap takes place, the returned iterator should just be the position
     * directly preceding 'itr', if such a position exists, since the stage at that position may be
     * able to perform further optimizations with its new neighbor.
     */
    virtual Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                           Pipeline::SourceContainer* container) {
        return std::next(itr);
    };

    enum GetDepsReturn {
        NOT_SUPPORTED = 0x0,      // The full object and all metadata may be required
        SEE_NEXT = 0x1,           // Later stages could need either fields or metadata
        EXHAUSTIVE_FIELDS = 0x2,  // Later stages won't need more fields from input
        EXHAUSTIVE_META = 0x4,    // Later stages won't need more metadata from input
        EXHAUSTIVE_ALL = EXHAUSTIVE_FIELDS | EXHAUSTIVE_META,  // Later stages won't need either
    };

    /**
     * Get the dependencies this operation needs to do its job.
     */
    virtual GetDepsReturn getDependencies(DepsTracker* deps) const {
        return NOT_SUPPORTED;
    }

    /**
     * In the default case, serializes the DocumentSource and adds it to the std::vector<Value>.
     *
     * A subclass may choose to overwrite this, rather than serialize,
     * if it should output multiple stages (eg, $sort sometimes also outputs a $limit).
     */

    virtual void serializeToArray(std::vector<Value>& array, bool explain = false) const;

    /**
     * Returns true if doesn't require an input source (most DocumentSources do).
     */
    virtual bool isValidInitialSource() const {
        return false;
    }

    /**
     * Returns true if the DocumentSource needs to be run on the primary shard.
     */
    virtual bool needsPrimaryShard() const {
        return false;
    }

    /**
     * If DocumentSource uses additional collections, it adds the namespaces to the input vector.
     */
    virtual void addInvolvedCollections(std::vector<NamespaceString>* collections) const {}

    virtual void detachFromOperationContext() {}

    virtual void reattachToOperationContext(OperationContext* opCtx) {}

    /**
     * Injects a new ExpressionContext into this DocumentSource and propagates the ExpressionContext
     * to all child expressions, accumulators, etc.
     *
     * Stages which require work to propagate the ExpressionContext to their private execution
     * machinery should override doInjectExpressionContext().
     */
    void injectExpressionContext(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        pExpCtx = expCtx;
        doInjectExpressionContext();
    }

    /**
     * Create a DocumentSource pipeline stage from 'stageObj'.
     */
    static std::vector<boost::intrusive_ptr<DocumentSource>> parse(
        const boost::intrusive_ptr<ExpressionContext> expCtx, BSONObj stageObj);

    /**
     * Registers a DocumentSource with a parsing function, so that when a stage with the given name
     * is encountered, it will call 'parser' to construct that stage.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_DOCUMENT_SOURCE macro defined in
     * this file.
     */
    static void registerParser(std::string name, Parser parser);

    /**
     * Given a BSONObj, construct a BSONObjSet consisting of all prefixes of that object. For
     * example, given {a: 1, b: 1, c: 1}, this will return a set: {{a: 1}, {a: 1, b: 1}, {a: 1, b:
     * 1, c: 1}}.
     */
    static BSONObjSet allPrefixes(BSONObj obj);

    /**
     * Given a BSONObjSet, where each BSONObj represents a sort key, return the BSONObjSet that
     * results from truncating each sort key before the first path that is a member of 'fields', or
     * is a child of a member of 'fields'.
     */
    static BSONObjSet truncateSortSet(const BSONObjSet& sorts, const std::set<std::string>& fields);

protected:
    /**
       Base constructor.
     */
    explicit DocumentSource(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * DocumentSources which need to update their internal state when attaching to a new
     * ExpressionContext should override this method.
     *
     * Any stage subclassing from DocumentSource should override this method if it contains
     * expressions or accumulators which need to attach to the newly injected ExpressionContext.
     */
    virtual void doInjectExpressionContext() {}

    /*
      Most DocumentSources have an underlying source they get their data
      from.  This is a convenience for them.

      The default implementation of setSource() sets this; if you don't
      need a source, override that to verify().  The default is to
      verify() if this has already been set.
    */
    DocumentSource* pSource;

    boost::intrusive_ptr<ExpressionContext> pExpCtx;

private:
    /**
     * Create a Value that represents the document source.
     *
     * This is used by the default implementation of serializeToArray() to add this object
     * to a pipeline being serialized. Returning a missing() Value results in no entry
     * being added to the array for this stage (DocumentSource).
     */
    virtual Value serialize(bool explain = false) const = 0;
};

/** This class marks DocumentSources that should be split between the merger and the shards.
 *  See Pipeline::Optimizations::Sharded::findSplitPoint() for details.
 */
class SplittableDocumentSource {
public:
    /** returns a source to be run on the shards.
     *  if NULL, don't run on shards
     */
    virtual boost::intrusive_ptr<DocumentSource> getShardSource() = 0;

    /** returns a source that combines results from shards.
     *  if NULL, don't run on merger
     */
    virtual boost::intrusive_ptr<DocumentSource> getMergeSource() = 0;

protected:
    // It is invalid to delete through a SplittableDocumentSource-typed pointer.
    virtual ~SplittableDocumentSource() {}
};


/** This class marks DocumentSources which need mongod-specific functionality.
 *  It causes a MongodInterface to be injected when in a mongod and prevents mongos from
 *  merging pipelines containing this stage.
 */
class DocumentSourceNeedsMongod : public DocumentSource {
public:
    // Wraps mongod-specific functions to allow linking into mongos.
    class MongodInterface {
    public:
        virtual ~MongodInterface(){};

        /**
         * Sets the OperationContext of the DBDirectClient returned by directClient(). This method
         * must be called after updating the 'opCtx' member of the ExpressionContext associated with
         * the document source.
         */
        virtual void setOperationContext(OperationContext* opCtx) = 0;

        /**
         * Always returns a DBDirectClient. The return type in the function signature is a
         * DBClientBase* because DBDirectClient isn't linked into mongos.
         */
        virtual DBClientBase* directClient() = 0;

        // Note that in some rare cases this could return a false negative but will never return
        // a false positive. This method will be fixed in the future once it becomes possible to
        // avoid false negatives.
        virtual bool isSharded(const NamespaceString& ns) = 0;

        /**
         * Inserts 'objs' into 'ns' and returns the "detailed" last error object.
         */
        virtual BSONObj insert(const NamespaceString& ns, const std::vector<BSONObj>& objs) = 0;

        virtual CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                                      const NamespaceString& ns) = 0;

        /**
         * Appends operation latency statistics for collection "nss" to "builder"
         */
        virtual void appendLatencyStats(const NamespaceString& nss,
                                        bool includeHistograms,
                                        BSONObjBuilder* builder) const = 0;

        /**
         * Appends storage statistics for collection "nss" to "builder"
         */
        virtual Status appendStorageStats(const NamespaceString& nss,
                                          const BSONObj& param,
                                          BSONObjBuilder* builder) const = 0;

        /**
         * Gets the collection options for the collection given by 'nss'.
         */
        virtual BSONObj getCollectionOptions(const NamespaceString& nss) = 0;

        /**
         * Performs the given rename command if the collection given by 'targetNs' has the same
         * options as specified in 'originalCollectionOptions', and has the same indexes as
         * 'originalIndexes'.
         */
        virtual Status renameIfOptionsAndIndexesHaveNotChanged(
            const BSONObj& renameCommandObj,
            const NamespaceString& targetNs,
            const BSONObj& originalCollectionOptions,
            const std::list<BSONObj>& originalIndexes) = 0;

        /**
         * Parses a Pipeline from a vector of BSONObjs representing DocumentSources and readies it
         * for execution. The returned pipeline is optimized and has a cursor source prepared.
         *
         * This function returns a non-OK status if parsing the pipeline failed.
         */
        virtual StatusWith<boost::intrusive_ptr<Pipeline>> makePipeline(
            const std::vector<BSONObj>& rawPipeline,
            const boost::intrusive_ptr<ExpressionContext>& expCtx) = 0;

        // Add new methods as needed.
    };

    DocumentSourceNeedsMongod(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(expCtx) {}

    void injectMongodInterface(std::shared_ptr<MongodInterface> mongod) {
        _mongod = mongod;
        doInjectMongodInterface(mongod);
    }

    /**
     * Derived classes may override this method to register custom inject functionality.
     */
    virtual void doInjectMongodInterface(std::shared_ptr<MongodInterface> mongod) {}

    void detachFromOperationContext() override {
        invariant(_mongod);
        _mongod->setOperationContext(nullptr);
        doDetachFromOperationContext();
    }

    /**
     * Derived classes may override this method to register custom detach functionality.
     */
    virtual void doDetachFromOperationContext() {}

    void reattachToOperationContext(OperationContext* opCtx) final {
        invariant(_mongod);
        _mongod->setOperationContext(opCtx);
        doReattachToOperationContext(opCtx);
    }

    /**
     * Derived classes may override this method to register custom reattach functionality.
     */
    virtual void doReattachToOperationContext(OperationContext* opCtx) {}

protected:
    // It is invalid to delete through a DocumentSourceNeedsMongod-typed pointer.
    virtual ~DocumentSourceNeedsMongod() {}

    // Gives subclasses access to a MongodInterface implementation
    std::shared_ptr<MongodInterface> _mongod;
};

/**
 * Constructs and returns Documents from the BSONObj objects produced by a supplied
 * PlanExecutor.
 *
 * An object of this type may only be used by one thread, see SERVER-6123.
 */
class DocumentSourceCursor final : public DocumentSource {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    BSONObjSet getOutputSorts() final {
        return _outputSorts;
    }
    /**
     * Attempts to combine with any subsequent $limit stages by setting the internal '_limit' field.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    Value serialize(bool explain = false) const final;
    bool isValidInitialSource() const final {
        return true;
    }
    void dispose() final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    /**
     * Create a document source based on a passed-in PlanExecutor.
     *
     * This is usually put at the beginning of a chain of document sources
     * in order to fetch data from the database.
     */
    static boost::intrusive_ptr<DocumentSourceCursor> create(
        const std::string& ns,
        std::unique_ptr<PlanExecutor> exec,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /*
      Record the query that was specified for the cursor this wraps, if
      any.

      This should be captured after any optimizations are applied to
      the pipeline so that it reflects what is really used.

      This gets used for explain output.

      @param pBsonObj the query to record
     */
    void setQuery(const BSONObj& query) {
        _query = query;
    }

    /*
      Record the sort that was specified for the cursor this wraps, if
      any.

      This should be captured after any optimizations are applied to
      the pipeline so that it reflects what is really used.

      This gets used for explain output.

      @param pBsonObj the sort to record
     */
    void setSort(const BSONObj& sort) {
        _sort = sort;
    }

    /**
     * Informs this object of projection and dependency information.
     *
     * @param projection The projection that has been passed down to the query system.
     * @param deps The output of DepsTracker::toParsedDeps.
     */
    void setProjection(const BSONObj& projection, const boost::optional<ParsedDeps>& deps);

    /// returns -1 for no limit
    long long getLimit() const;

    /**
     * If subsequent sources need no information from the cursor, the cursor can simply output empty
     * documents, avoiding the overhead of converting BSONObjs to Documents.
     */
    void shouldProduceEmptyDocs() {
        _shouldProduceEmptyDocs = true;
    }

    const std::string& getPlanSummaryStr() const;

    const PlanSummaryStats& getPlanSummaryStats() const;

protected:
    void doInjectExpressionContext() final;

private:
    DocumentSourceCursor(const std::string& ns,
                         std::unique_ptr<PlanExecutor> exec,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    void loadBatch();

    void recordPlanSummaryStr();

    void recordPlanSummaryStats();

    std::deque<Document> _currentBatch;

    // BSONObj members must outlive _projection and cursor.
    BSONObj _query;
    BSONObj _sort;
    BSONObj _projection;
    bool _shouldProduceEmptyDocs = false;
    boost::optional<ParsedDeps> _dependencies;
    boost::intrusive_ptr<DocumentSourceLimit> _limit;
    long long _docsAddedToBatches;  // for _limit enforcement

    const std::string _ns;
    std::unique_ptr<PlanExecutor> _exec;
    BSONObjSet _outputSorts;
    std::string _planSummary;
    PlanSummaryStats _planSummaryStats;
};


class DocumentSourceGroup final : public DocumentSource, public SplittableDocumentSource {
public:
    using Accumulators = std::vector<boost::intrusive_ptr<Accumulator>>;
    using GroupsMap = ValueUnorderedMap<Accumulators>;

    static const size_t kDefaultMaxMemoryUsageBytes = 100 * 1024 * 1024;

    // Virtuals from DocumentSource.
    boost::intrusive_ptr<DocumentSource> optimize() final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;
    Value serialize(bool explain = false) const final;
    GetNextResult getNext() final;
    void dispose() final;
    const char* getSourceName() const final;
    BSONObjSet getOutputSorts() final;

    /**
     * Convenience method for creating a new $group stage.
     */
    static boost::intrusive_ptr<DocumentSourceGroup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        std::vector<AccumulationStatement> accumulationStatements,
        Variables::Id numVariables,
        size_t maxMemoryUsageBytes = kDefaultMaxMemoryUsageBytes);

    /**
     * Parses 'elem' into a $group stage, or throws a UserException if 'elem' was an invalid
     * specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Add an accumulator, which will become a field in each Document that results from grouping.
     */
    void addAccumulator(AccumulationStatement accumulationStatement);

    /**
     * Sets the expression to use to determine the group id of each document.
     */
    void setIdExpression(const boost::intrusive_ptr<Expression> idExpression);

    /**
     * Tell this source if it is doing a merge from shards. Defaults to false.
     */
    void setDoingMerge(bool doingMerge) {
        _doingMerge = doingMerge;
    }

    bool isStreaming() const {
        return _streaming;
    }

    // Virtuals for SplittableDocumentSource.
    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

protected:
    void doInjectExpressionContext() final;

private:
    explicit DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                 size_t maxMemoryUsageBytes = kDefaultMaxMemoryUsageBytes);

    /**
     * getNext() dispatches to one of these three depending on what type of $group it is. All three
     * of these methods expect '_currentAccumulators' to have been reset before being called, and
     * also expect initialize() to have been called already.
     */
    GetNextResult getNextStreaming();
    GetNextResult getNextSpilled();
    GetNextResult getNextStandard();

    /**
     * Attempt to identify an input sort order that allows us to turn into a streaming $group. If we
     * find one, return it. Otherwise, return boost::none.
     */
    boost::optional<BSONObj> findRelevantInputSort() const;

    /**
     * Before returning anything, this source must prepare itself. In a streaming $group,
     * initialize() requests the first document from the previous source, and uses it to prepare the
     * accumulators. In an unsorted $group, initialize() exhausts the previous source before
     * returning. The '_initialized' boolean indicates that initialize() has finished.
     *
     * This method may not be able to finish initialization in a single call if 'pSource' returns a
     * DocumentSource::GetNextResult::kPauseExecution, so it returns the last GetNextResult
     * encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult initialize();

    /**
     * Spill groups map to disk and returns an iterator to the file. Note: Since a sorted $group
     * does not exhaust the previous stage before returning, and thus does not maintain as large a
     * store of documents at any one time, only an unsorted group can spill to disk.
     */
    std::shared_ptr<Sorter<Value, Value>::Iterator> spill();

    Document makeDocument(const Value& id, const Accumulators& accums, bool mergeableOutput);

    /**
     * Computes the internal representation of the group key.
     */
    Value computeId(Variables* vars);

    /**
     * Converts the internal representation of the group key to the _id shape specified by the
     * user.
     */
    Value expandId(const Value& val);

    /**
     * 'vFieldName' contains the field names for the result documents, 'vpAccumulatorFactory'
     * contains the accumulator factories for the result documents, and 'vpExpression' contains the
     * common expressions used by each instance of each accumulator in order to find the right-hand
     * side of what gets added to the accumulator. These three vectors parallel each other.
     */
    std::vector<std::string> vFieldName;
    std::vector<Accumulator::Factory> vpAccumulatorFactory;
    std::vector<boost::intrusive_ptr<Expression>> vpExpression;

    bool _doingMerge;
    size_t _memoryUsageBytes = 0;
    size_t _maxMemoryUsageBytes;
    std::unique_ptr<Variables> _variables;
    std::vector<std::string> _idFieldNames;  // used when id is a document
    std::vector<boost::intrusive_ptr<Expression>> _idExpressions;

    BSONObj _inputSort;
    bool _streaming;
    bool _initialized;

    Value _currentId;
    Accumulators _currentAccumulators;

    // We use boost::optional to defer initialization until the ExpressionContext containing the
    // correct comparator is injected, since the groups must be built using the comparator's
    // definition of equality.
    boost::optional<GroupsMap> _groups;

    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> _sortedFiles;
    bool _spilled;

    // Only used when '_spilled' is false.
    GroupsMap::iterator groupsIterator;

    // Only used when '_spilled' is true.
    std::unique_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;
    const bool _extSortAllowed;

    std::pair<Value, Value> _firstPartOfNextGroup;
    // Only used when '_sorted' is true.
    boost::optional<Document> _firstDocOfNextGroup;
};

/**
 * Provides a document source interface to retrieve index statistics for a given namespace.
 * Each document returned represents a single index and mongod instance.
 */
class DocumentSourceIndexStats final : public DocumentSourceNeedsMongod {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(bool explain = false) const final;

    virtual bool isValidInitialSource() const final {
        return true;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceIndexStats(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    CollectionIndexUsageMap _indexStatsMap;
    CollectionIndexUsageMap::const_iterator _indexStatsIter;
    std::string _processName;
};

class DocumentSourceMatch final : public DocumentSource {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(bool explain = false) const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    BSONObjSet getOutputSorts() final {
        return pSource ? pSource->getOutputSorts()
                       : SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }
    /**
     * Attempts to combine with any subsequent $match stages, joining the query objects with a
     * $and.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    void setSource(DocumentSource* Source) final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    /**
     * Convenience method for creating a $match stage.
     */
    static boost::intrusive_ptr<DocumentSourceMatch> create(
        BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses a $match stage from 'elem'.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    /**
     * Access the MatchExpression stored inside the DocumentSourceMatch. Does not release ownership.
     */
    MatchExpression* getMatchExpression() const {
        return _expression.get();
    }

    /**
     * Combines the filter in this $match with the filter of 'other' using a $and, updating this
     * match in place.
     */
    void joinMatchWith(boost::intrusive_ptr<DocumentSourceMatch> other);

    /**
     * Returns the query in MatchExpression syntax.
     */
    BSONObj getQuery() const;

    /** Returns the portion of the match that can safely be promoted to before a $redact.
     *  If this returns an empty BSONObj, no part of this match may safely be promoted.
     *
     *  To be safe to promote, removing a field from a document to be matched must not cause
     *  that document to be accepted when it would otherwise be rejected. As an example,
     *  {name: {$ne: "bob smith"}} accepts documents without a name field, which means that
     *  running this filter before a redact that would remove the name field would leak
     *  information. On the other hand, {age: {$gt:5}} is ok because it doesn't accept documents
     *  that have had their age field removed.
     */
    BSONObj redactSafePortion() const;

    static bool isTextQuery(const BSONObj& query);
    bool isTextQuery() const {
        return _isTextQuery;
    }

    /**
     * Attempt to split this $match into two stages, where the first is not dependent upon any path
     * from 'fields', and where applying them in sequence is equivalent to applying this stage once.
     *
     * Will return two intrusive_ptrs to new $match stages, where the first pointer is independent
     * of 'fields', and the second is dependent. Either pointer may be null, so be sure to check the
     * return value.
     *
     * For example, {$match: {a: "foo", "b.c": 4}} split by "b" will return pointers to two stages:
     * {$match: {a: "foo"}}, and {$match: {"b.c": 4}}.
     */
    std::pair<boost::intrusive_ptr<DocumentSource>, boost::intrusive_ptr<DocumentSource>>
    splitSourceBy(const std::set<std::string>& fields);

    /**
     * Given a document 'input', extract 'fields' and produce a BSONObj with those values.
     */
    static BSONObj getObjectForMatch(const Document& input, const std::set<std::string>& fields);

    /**
     * Should be called _only_ on a MatchExpression  that is a predicate on 'path', or subfields  of
     * 'path'. It is also invalid to call this method on a $match including a $elemMatch on 'path',
     * for example: {$match: {'path': {$elemMatch: {'subfield': 3}}}}
     *
     * Returns a new DocumentSourceMatch that, if executed on the subdocument at 'path', is
     * equivalent to 'expression'.
     *
     * For example, if the original expression is {$and: [{'a.b': {$gt: 0}}, {'a.d': {$eq: 3}}]},
     * the new $match will have the expression {$and: [{b: {$gt: 0}}, {d: {$eq: 3}}]} after
     * descending on the path 'a'.
     */
    static boost::intrusive_ptr<DocumentSourceMatch> descendMatchOnPath(
        MatchExpression* matchExpr,
        const std::string& path,
        boost::intrusive_ptr<ExpressionContext> expCtx);

    void doInjectExpressionContext();

private:
    DocumentSourceMatch(const BSONObj& query,
                        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    void addDependencies(DepsTracker* deps) const;

    std::unique_ptr<MatchExpression> _expression;

    // Cache the dependencies so that we know what fields we need to serialize to BSON for matching.
    DepsTracker _dependencies;

    BSONObj _predicate;
    bool _isTextQuery;
};

class DocumentSourceMergeCursors : public DocumentSource {
public:
    struct CursorDescriptor {
        CursorDescriptor(ConnectionString connectionString, std::string ns, CursorId cursorId)
            : connectionString(std::move(connectionString)),
              ns(std::move(ns)),
              cursorId(cursorId) {}

        ConnectionString connectionString;
        std::string ns;
        CursorId cursorId;
    };

    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void dispose() final;
    Value serialize(bool explain = false) const final;
    bool isValidInitialSource() const final {
        return true;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSource> create(
        std::vector<CursorDescriptor> cursorDescriptors,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /** Returns non-owning pointers to cursors managed by this stage.
     *  Call this instead of getNext() if you want access to the raw streams.
     *  This method should only be called at most once.
     */
    std::vector<DBClientCursor*> getCursors();

    /**
     * Returns the next object from the cursor, throwing an appropriate exception if the cursor
     * reported an error. This is a better form of DBClientCursor::nextSafe.
     */
    static Document nextSafeFrom(DBClientCursor* cursor);

private:
    struct CursorAndConnection {
        CursorAndConnection(const CursorDescriptor& cursorDescriptor);
        ScopedDbConnection connection;
        DBClientCursor cursor;
    };

    // using list to enable removing arbitrary elements
    typedef std::list<std::shared_ptr<CursorAndConnection>> Cursors;

    DocumentSourceMergeCursors(std::vector<CursorDescriptor> cursorDescriptors,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    // Converts _cursorDescriptors into active _cursors.
    void start();

    // This is the description of cursors to merge.
    const std::vector<CursorDescriptor> _cursorDescriptors;

    // These are the actual cursors we are merging. Created lazily.
    Cursors _cursors;
    Cursors::iterator _currentCursor;

    bool _unstarted;
};

/**
 * Used in testing to store documents without using the storage layer. Methods are not marked as
 * final in order to allow tests to intercept calls if needed.
 */
class DocumentSourceMock : public DocumentSource {
public:
    DocumentSourceMock(std::deque<GetNextResult> results);
    DocumentSourceMock(std::deque<GetNextResult> results,
                       const boost::intrusive_ptr<ExpressionContext>& expCtx);

    GetNextResult getNext() override;
    const char* getSourceName() const override;
    Value serialize(bool explain = false) const override;
    void dispose() override;
    bool isValidInitialSource() const override {
        return true;
    }
    BSONObjSet getOutputSorts() override {
        return sorts;
    }

    static boost::intrusive_ptr<DocumentSourceMock> create();

    static boost::intrusive_ptr<DocumentSourceMock> create(Document doc);

    static boost::intrusive_ptr<DocumentSourceMock> create(const GetNextResult& result);
    static boost::intrusive_ptr<DocumentSourceMock> create(std::deque<GetNextResult> results);

    static boost::intrusive_ptr<DocumentSourceMock> create(const char* json);
    static boost::intrusive_ptr<DocumentSourceMock> create(
        const std::initializer_list<const char*>& jsons);

    void reattachToOperationContext(OperationContext* opCtx) {
        isDetachedFromOpCtx = false;
    }

    void detachFromOperationContext() {
        isDetachedFromOpCtx = true;
    }

    boost::intrusive_ptr<DocumentSource> optimize() override {
        isOptimized = true;
        return this;
    }

    void doInjectExpressionContext() override {
        isExpCtxInjected = true;
    }

    // Return documents from front of queue.
    std::deque<GetNextResult> queue;

    bool isDisposed = false;
    bool isDetachedFromOpCtx = false;
    bool isOptimized = false;
    bool isExpCtxInjected = false;

    BSONObjSet sorts;
};

/**
 * This class is for DocumentSources that take in and return one document at a time, in a 1:1
 * transformation. It should only be used via an alias that passes the transformation logic through
 * a ParsedSingleDocumentTransformation. It is not a registered DocumentSource, and it cannot be
 * created from BSON.
 */
class DocumentSourceSingleDocumentTransformation final : public DocumentSource {
public:
    /**
     * This class defines the minimal interface that every parser wishing to take advantage of
     * DocumentSourceSingleDocumentTransformation must implement.
     *
     * This interface ensures that DocumentSourceSingleDocumentTransformations are passed parsed
     * objects that can execute the transformation and provide additional features like
     * serialization and reporting and returning dependencies. The parser must also provide
     * implementations for optimizing and adding the expression context, even if those functions do
     * nothing.
     */
    class TransformerInterface {
    public:
        virtual ~TransformerInterface() = default;
        virtual Document applyTransformation(Document input) = 0;
        virtual void optimize() = 0;
        virtual Document serialize(bool explain) const = 0;
        virtual DocumentSource::GetDepsReturn addDependencies(DepsTracker* deps) const = 0;
        virtual void injectExpressionContext(
            const boost::intrusive_ptr<ExpressionContext>& pExpCtx) = 0;
    };

    DocumentSourceSingleDocumentTransformation(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        std::unique_ptr<TransformerInterface> parsedTransform,
        std::string name);

    // virtuals from DocumentSource
    const char* getSourceName() const final;
    GetNextResult getNext() final;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    void dispose() final;
    Value serialize(bool explain) const final;
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    void doInjectExpressionContext() final;
    DocumentSource::GetDepsReturn getDependencies(DepsTracker* deps) const final;

private:
    // Stores transformation logic.
    std::unique_ptr<TransformerInterface> _parsedTransform;

    // Specific name of the transformation.
    std::string _name;
};

class DocumentSourceOut final : public DocumentSourceNeedsMongod, public SplittableDocumentSource {
public:
    // virtuals from DocumentSource
    ~DocumentSourceOut() final;
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(bool explain = false) const final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;
    bool needsPrimaryShard() const final {
        return true;
    }

    // Virtuals for SplittableDocumentSource
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return NULL;
    }
    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    const NamespaceString& getOutputNs() const {
        return _outputNs;
    }

    /**
      Create a document source for output and pass-through.

      This can be put anywhere in a pipeline and will store content as
      well as pass it on.

      @param pBsonElement the raw BSON specification for the source
      @param pExpCtx the expression context for the pipeline
      @returns the newly created document source
    */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceOut(const NamespaceString& outputNs,
                      const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Sets '_tempNs' to a unique temporary namespace, makes sure the output collection isn't
     * sharded or capped, and saves the collection options and indexes of the target collection.
     * Then creates the temporary collection we will insert into by copying the collection options
     * and indexes from the target collection.
     *
     * Sets '_initialized' to true upon completion.
     */
    void initialize();

    /**
     * Inserts all of 'toInsert' into the temporary collection.
     */
    void spill(const std::vector<BSONObj>& toInsert);

    bool _initialized = false;
    bool _done = false;

    // Holds on to the original collection options and index specs so we can check they didn't
    // change during computation.
    BSONObj _originalOutOptions;
    std::list<BSONObj> _originalIndexes;

    NamespaceString _tempNs;          // output goes here as it is being processed.
    const NamespaceString _outputNs;  // output will go here after all data is processed.
};

class DocumentSourceRedact final : public DocumentSource {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;

    /**
     * Attempts to duplicate the redact-safe portion of a subsequent $match before the $redact
     * stage.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;

    void doInjectExpressionContext() final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Value serialize(bool explain = false) const final;

private:
    DocumentSourceRedact(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const boost::intrusive_ptr<Expression>& previsit);

    // These both work over _variables
    boost::optional<Document> redactObject();  // redacts CURRENT
    Value redactValue(const Value& in);

    Variables::Id _currentId;
    std::unique_ptr<Variables> _variables;
    boost::intrusive_ptr<Expression> _expression;
};

/*
 * $replaceRoot takes an object containing only an expression in the newRoot field, and replaces
 * each incoming document with the result of evaluating that expression. Throws an error if the
 * given expression is not an object or if the expression evaluates to the "missing" Value. This
 * is implemented as an extension of DocumentSourceSingleDocumentTransformation.
 */
class DocumentSourceReplaceRoot final {
public:
    /**
     * Creates a new replaceRoot DocumentSource from the BSON specification of the $replaceRoot
     * stage.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceReplaceRoot() = default;
};

class DocumentSourceSample final : public DocumentSource, public SplittableDocumentSource {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(bool explain = false) const final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return SEE_NEXT;
    }

    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

    long long getSampleSize() const {
        return _size;
    }

    void doInjectExpressionContext() final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    explicit DocumentSourceSample(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    long long _size;

    // Uses a $sort stage to randomly sort the documents.
    boost::intrusive_ptr<DocumentSourceSort> _sortStage;
};

/**
 * This class is not a registered stage, it is only used as an optimized replacement for $sample
 * when the storage engine allows us to use a random cursor.
 */
class DocumentSourceSampleFromRandomCursor final : public DocumentSource {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(bool explain = false) const final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    void doInjectExpressionContext() final;

    static boost::intrusive_ptr<DocumentSourceSampleFromRandomCursor> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        long long size,
        std::string idField,
        long long collectionSize);

private:
    DocumentSourceSampleFromRandomCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         long long size,
                                         std::string idField,
                                         long long collectionSize);

    /**
     * Keep asking for documents from the random cursor until it yields a new document. Errors if a
     * a document is encountered without a value for '_idField', or if the random cursor keeps
     * returning duplicate elements.
     */
    GetNextResult getNextNonDuplicateDocument();

    long long _size;

    // The field to use as the id of a document. Usually '_id', but 'ts' for the oplog.
    std::string _idField;

    // Keeps track of the documents that have been returned, since a random cursor is allowed to
    // return duplicates. We use boost::optional to defer initialization until the ExpressionContext
    // containing the correct comparator is injected.
    boost::optional<ValueUnorderedSet> _seenDocs;

    // The approximate number of documents in the collection (includes orphans).
    const long long _nDocsInColl;

    // The value to be assigned to the randMetaField of outcoming documents. Each call to getNext()
    // will decrement this value by an amount scaled by _nDocsInColl as an attempt to appear as if
    // the documents were produced by a top-k random sort.
    double _randMetaFieldVal = 1.0;
};

class DocumentSourceLimit final : public DocumentSource, public SplittableDocumentSource {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    BSONObjSet getOutputSorts() final {
        return pSource ? pSource->getOutputSorts()
                       : SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    /**
     * Attempts to combine with a subsequent $limit stage, setting 'limit' appropriately.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    Value serialize(bool explain = false) const final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return SEE_NEXT;  // This doesn't affect needed fields
    }

    /**
      Create a new limiting DocumentSource.

      @param pExpCtx the expression context for the pipeline
      @returns the DocumentSource
     */
    static boost::intrusive_ptr<DocumentSourceLimit> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    // Virtuals for SplittableDocumentSource
    // Need to run on rounter. Running on shard as well is an optimization.
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return this;
    }
    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    long long getLimit() const {
        return _limit;
    }
    void setLimit(long long newLimit) {
        _limit = newLimit;
    }

    /**
      Create a limiting DocumentSource from BSON.

      This is a convenience method that uses the above, and operates on
      a BSONElement that has been deteremined to be an Object with an
      element named $limit.

      @param pBsonElement the BSONELement that defines the limit
      @param pExpCtx the expression context
      @returns the grouping DocumentSource
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceLimit(const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    long long _limit;
    long long _nReturned = 0;
};

class DocumentSourceSort final : public DocumentSource, public SplittableDocumentSource {
public:
    static const uint64_t kMaxMemoryUsageBytes = 100 * 1024 * 1024;

    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void serializeToArray(std::vector<Value>& array, bool explain = false) const final;

    BSONObjSet getOutputSorts() final {
        return allPrefixes(_sort);
    }

    /**
     * Attempts to move a subsequent $match stage before the $sort, reducing the number of
     * documents that pass through the stage. Also attempts to absorb a subsequent $limit stage so
     * that it an perform a top-k sort.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    void dispose() final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

    /// Write out a Document whose contents are the sort key.
    Document serializeSortKey(bool explain) const;

    /**
     * Parses a $sort stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Convenience method for creating a $sort stage.
     */
    static boost::intrusive_ptr<DocumentSourceSort> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        BSONObj sortOrder,
        long long limit = -1,
        uint64_t maxMemoryUsageBytes = kMaxMemoryUsageBytes);

    /**
     * Returns -1 for no limit.
     */
    long long getLimit() const;

    /**
     * Loads a document to be sorted. This can be used to sort a stream of documents that are not
     * coming from another DocumentSource. Once all documents have been added, the caller must call
     * loadingDone() before using getNext() to receive the documents in sorted order.
     */
    void loadDocument(const Document& doc);

    /**
     * Signals to the sort stage that there will be no more input documents. It is an error to call
     * loadDocument() once this method returns.
     */
    void loadingDone();

    /**
     * Instructs the sort stage to use the given set of cursors as inputs, to merge documents that
     * have already been sorted.
     */
    void populateFromCursors(const std::vector<DBClientCursor*>& cursors);

    bool isPopulated() {
        return _populated;
    };

    boost::intrusive_ptr<DocumentSourceLimit> getLimitSrc() const {
        return limitSrc;
    }

private:
    explicit DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(bool explain = false) const final {
        MONGO_UNREACHABLE;  // Should call serializeToArray instead.
    }

    /**
     * Helper to add a sort key to this stage.
     */
    void addKey(StringData fieldPath, bool ascending);

    /**
     * Before returning anything, we have to consume all input and sort it. This method consumes all
     * input and prepares the sorted stream '_output'.
     *
     * This method may not be able to finish populating the sorter in a single call if 'pSource'
     * returns a DocumentSource::GetNextResult::kPauseExecution, so it returns the last
     * GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populate();
    bool _populated = false;

    BSONObj _sort;

    SortOptions makeSortOptions() const;

    // This is used to merge pre-sorted results from a DocumentSourceMergeCursors.
    class IteratorFromCursor;

    /* these two parallel each other */
    typedef std::vector<boost::intrusive_ptr<Expression>> SortKey;
    SortKey vSortKey;
    std::vector<char> vAscending;  // used like std::vector<bool> but without specialization

    /// Extracts the fields in vSortKey from the Document;
    Value extractKey(const Document& d) const;

    /// Compare two Values according to the specified sort key.
    int compare(const Value& lhs, const Value& rhs) const;

    typedef Sorter<Value, Document> MySorter;

    /**
     * Absorbs 'limit', enabling a top-k sort. It is safe to call this multiple times, it will keep
     * the smallest limit.
     */
    void setLimitSrc(boost::intrusive_ptr<DocumentSourceLimit> limit) {
        if (!limitSrc || limit->getLimit() < limitSrc->getLimit()) {
            limitSrc = limit;
        }
    }

    // For MySorter
    class Comparator {
    public:
        explicit Comparator(const DocumentSourceSort& source) : _source(source) {}
        int operator()(const MySorter::Data& lhs, const MySorter::Data& rhs) const {
            return _source.compare(lhs.first, rhs.first);
        }

    private:
        const DocumentSourceSort& _source;
    };

    boost::intrusive_ptr<DocumentSourceLimit> limitSrc;

    uint64_t _maxMemoryUsageBytes;
    bool _done;
    bool _mergingPresorted;
    std::unique_ptr<MySorter> _sorter;
    std::unique_ptr<MySorter::Iterator> _output;
};

class DocumentSourceSkip final : public DocumentSource, public SplittableDocumentSource {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    /**
     * Attempts to move a subsequent $limit before the skip, potentially allowing for forther
     * optimizations earlier in the pipeline.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    Value serialize(bool explain = false) const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    BSONObjSet getOutputSorts() final {
        return pSource ? pSource->getOutputSorts()
                       : SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return SEE_NEXT;  // This doesn't affect needed fields
    }

    // Virtuals for SplittableDocumentSource
    // Need to run on rounter. Can't run on shards.
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return NULL;
    }
    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    long long getSkip() const {
        return _nToSkip;
    }
    void setSkip(long long newSkip) {
        _nToSkip = newSkip;
    }

    /**
     * Convenience method for creating a $skip stage.
     */
    static boost::intrusive_ptr<DocumentSourceSkip> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long nToSkip);

    /**
     * Parses the user-supplied BSON into a $skip stage.
     *
     * Throws a UserException if 'elem' is an invalid $skip specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    explicit DocumentSourceSkip(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                long long nToSkip);

    long long _nToSkip = 0;
    long long _nSkippedSoFar = 0;
};


class DocumentSourceUnwind final : public DocumentSource {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(bool explain = false) const final;
    BSONObjSet getOutputSorts() final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    /**
     * If the next stage is a $match, the part of the match that is not dependent on the unwound
     * field can be moved into a new, preceding, $match stage.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;

    /**
     * Creates a new $unwind DocumentSource from a BSON specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceUnwind> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::string& path,
        bool includeNullIfEmptyOrMissing,
        const boost::optional<std::string>& includeArrayIndex);

    std::string getUnwindPath() const {
        return _unwindPath.fullPath();
    }

    bool preserveNullAndEmptyArrays() const {
        return _preserveNullAndEmptyArrays;
    }

    const boost::optional<FieldPath>& indexPath() const {
        return _indexPath;
    }

private:
    DocumentSourceUnwind(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         const FieldPath& fieldPath,
                         bool includeNullIfEmptyOrMissing,
                         const boost::optional<FieldPath>& includeArrayIndex);

    // Configuration state.
    const FieldPath _unwindPath;
    // Documents that have a nullish value, or an empty array for the field '_unwindPath', will pass
    // through the $unwind stage unmodified if '_preserveNullAndEmptyArrays' is true.
    const bool _preserveNullAndEmptyArrays;
    // If set, the $unwind stage will include the array index in the specified path, overwriting any
    // existing value, setting to null when the value was a non-array or empty array.
    const boost::optional<FieldPath> _indexPath;

    // Iteration state.
    class Unwinder;
    std::unique_ptr<Unwinder> _unwinder;
};

class DocumentSourceGeoNear : public DocumentSourceNeedsMongod, public SplittableDocumentSource {
public:
    static const long long kDefaultLimit;

    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    /**
     * Attempts to combine with a subsequent limit stage, setting the internal limit field
     * as a result.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    bool isValidInitialSource() const final {
        return true;
    }
    Value serialize(bool explain = false) const final;
    BSONObjSet getOutputSorts() final {
        return SimpleBSONObjComparator::kInstance.makeBSONObjSet(
            {BSON(distanceField->fullPath() << -1)});
    }

    // Virtuals for SplittableDocumentSource
    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    static char geoNearName[];

    long long getLimit() {
        return limit;
    }

    BSONObj getQuery() const {
        return query;
    };

    // this should only be used for testing
    static boost::intrusive_ptr<DocumentSourceGeoNear> create(
        const boost::intrusive_ptr<ExpressionContext>& pCtx);

private:
    explicit DocumentSourceGeoNear(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    void parseOptions(BSONObj options);
    BSONObj buildGeoNearCmd() const;
    void runCommand();

    // These fields describe the command to run.
    // coords and distanceField are required, rest are optional
    BSONObj coords;  // "near" option, but near is a reserved keyword on windows
    bool coordsIsArray;
    std::unique_ptr<FieldPath> distanceField;  // Using unique_ptr because FieldPath can't be empty
    long long limit;
    double maxDistance;
    double minDistance;
    BSONObj query;
    bool spherical;
    double distanceMultiplier;
    std::unique_ptr<FieldPath> includeLocs;

    // these fields are used while processing the results
    BSONObj cmdOutput;
    std::unique_ptr<BSONObjIterator> resultsIterator;  // iterator over cmdOutput["results"]
};

/**
 * Queries separate collection for equality matches with documents in the pipeline collection.
 * Adds matching documents to a new array field in the input document.
 */
class DocumentSourceLookUp final : public DocumentSourceNeedsMongod,
                                   public SplittableDocumentSource {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void serializeToArray(std::vector<Value>& array, bool explain = false) const final;
    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwindSrc'
     * field.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;
    void dispose() final;

    BSONObjSet getOutputSorts() final {
        return DocumentSource::truncateSortSet(pSource->getOutputSorts(), {_as.fullPath()});
    }

    bool needsPrimaryShard() const final {
        return true;
    }

    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }

    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    void addInvolvedCollections(std::vector<NamespaceString>* collections) const final {
        collections->push_back(_fromNs);
    }

    void doDetachFromOperationContext() final;

    void doReattachToOperationContext(OperationContext* opCtx) final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

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
        invariant(!_handlingUnwind);
        _unwindSrc = unwind;
        _handlingUnwind = true;
    }

protected:
    void doInjectExpressionContext() final;

private:
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::string localField,
                         std::string foreignField,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(bool explain = false) const final {
        // Should not be called; use serializeToArray instead.
        MONGO_UNREACHABLE;
    }

    GetNextResult unwindResult();

    NamespaceString _fromNs;
    FieldPath _as;
    FieldPath _localField;
    FieldPath _foreignField;
    std::string _foreignFieldFieldName;
    boost::optional<BSONObj> _additionalFilter;

    // The ExpressionContext used when performing aggregation pipelines against the '_fromNs'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // The aggregation pipeline to perform against the '_fromNs' namespace.
    std::vector<BSONObj> _fromPipeline;

    boost::intrusive_ptr<DocumentSourceMatch> _matchSrc;
    boost::intrusive_ptr<DocumentSourceUnwind> _unwindSrc;

    bool _handlingUnwind = false;
    bool _handlingMatch = false;

    // The following members are used to hold onto state across getNext() calls when
    // '_handlingUnwind' is true.
    long long _cursorIndex = 0;
    boost::intrusive_ptr<Pipeline> _pipeline;
    boost::optional<Document> _input;
    boost::optional<Document> _nextValue;
};

class DocumentSourceGraphLookUp final : public DocumentSourceNeedsMongod {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void dispose() final;
    BSONObjSet getOutputSorts() final;
    void serializeToArray(std::vector<Value>& array, bool explain = false) const final;

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwind' field.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        _startWith->addDependencies(deps);
        return SEE_NEXT;
    };

    bool needsPrimaryShard() const final {
        return true;
    }

    void addInvolvedCollections(std::vector<NamespaceString>* collections) const final {
        collections->push_back(_from);
    }

    void doDetachFromOperationContext() final;

    void doReattachToOperationContext(OperationContext* opCtx) final;

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

protected:
    void doInjectExpressionContext() final;

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

    Value serialize(bool explain = false) const final {
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
    boost::optional<BSONObj> makeMatchStageFromFrontier(BSONObjSet* cached);

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
    void addToCache(const BSONObj& result, const ValueUnorderedSet& queried);

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
    bool addToVisitedAndFrontier(BSONObj result, long long depth);

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
    // We use boost::optional to defer initialization until the ExpressionContext containing the
    // correct comparator is injected.
    boost::optional<ValueUnorderedSet> _frontier;

    // Tracks nodes that have been discovered for a given input. Keys are the '_id' value of the
    // document from the foreign collection, value is the document itself.  The keys are compared
    // using the simple collation.
    ValueUnorderedMap<BSONObj> _visited;

    // Caches query results to avoid repeating any work. This structure is maintained across calls
    // to getNext().
    LookupSetCache _cache;

    // When we have internalized a $unwind, we must keep track of the input document, since we will
    // need it for multiple "getNext()" calls.
    boost::optional<Document> _input;

    // The variables that are in scope to be used by the '_startWith' expression.
    std::unique_ptr<Variables> _variables;

    // Keep track of a $unwind that was absorbed into this stage.
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> _unwind;

    // If we absorbed a $unwind that specified 'includeArrayIndex', this is used to populate that
    // field, tracking how many results we've returned so far for the current input document.
    long long _outputIndex;
};

class DocumentSourceSortByCount final {
public:
    static std::vector<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceSortByCount() = default;
};

class DocumentSourceCount final {
public:
    static std::vector<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceCount() = default;
};

class DocumentSourceBucket final {
public:
    static std::vector<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceBucket() = default;
};

/**
 * The $project stage can be used for simple transformations such as including or excluding a set
 * of fields, or can do more sophisticated things, like include some fields and add new "computed"
 * fields, using the expression language. Note you can not mix an exclusion-style projection with
 * adding or including any other fields.
 */
class DocumentSourceProject final {
public:
    /**
     * Convenience method to create a $project stage from 'projectSpec'.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        BSONObj projectSpec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses a $project stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceProject() = default;
};

/**
 * $addFields adds or replaces the specified fields to/in the document while preserving the original
 * document. It is modeled on and throws the same errors as $project.
 */
class DocumentSourceAddFields final {
public:
    /**
     * Convenience method for creating a $addFields stage from 'addFieldsSpec'.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        BSONObj addFieldsSpec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses a $addFields stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    DocumentSourceAddFields() = default;
};

/**
 * Provides a document source interface to retrieve collection-level statistics for a given
 * collection.
 */
class DocumentSourceCollStats : public DocumentSourceNeedsMongod {
public:
    DocumentSourceCollStats(const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
        : DocumentSourceNeedsMongod(pExpCtx) {}

    GetNextResult getNext() final;

    const char* getSourceName() const final;

    bool isValidInitialSource() const final;

    Value serialize(bool explain = false) const;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // The raw object given to $collStats containing user specified options.
    BSONObj _collStatsSpec;
    bool _finished = false;
};

/**
 * The $bucketAuto stage takes a user-specified number of buckets and automatically determines
 * boundaries such that the values are approximately equally distributed between those buckets.
 */
class DocumentSourceBucketAuto final : public DocumentSource, public SplittableDocumentSource {
public:
    Value serialize(bool explain = false) const final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;
    GetNextResult getNext() final;
    void dispose() final;
    const char* getSourceName() const final;

    /**
     * The $bucketAuto stage must be run on the merging shard.
     */
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }
    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    static const uint64_t kDefaultMaxMemoryUsageBytes = 100 * 1024 * 1024;

    /**
     * Convenience method to create a $bucketAuto stage.
     *
     * If 'accumulationStatements' is the empty vector, it will be filled in with the statement
     * 'count: {$sum: 1}'.
     */
    static boost::intrusive_ptr<DocumentSourceBucketAuto> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        Variables::Id numVariables,
        int numBuckets,
        std::vector<AccumulationStatement> accumulationStatements = {},
        const boost::intrusive_ptr<GranularityRounder>& granularityRounder = nullptr,
        uint64_t maxMemoryUsageBytes = kDefaultMaxMemoryUsageBytes);

    /**
     * Parses a $bucketAuto stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceBucketAuto(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                             const boost::intrusive_ptr<Expression>& groupByExpression,
                             Variables::Id numVariables,
                             int numBuckets,
                             std::vector<AccumulationStatement> accumulationStatements,
                             const boost::intrusive_ptr<GranularityRounder>& granularityRounder,
                             uint64_t maxMemoryUsageBytes);

    // struct for holding information about a bucket.
    struct Bucket {
        Bucket(Value min, Value max, std::vector<Accumulator::Factory> accumulatorFactories);
        Value _min;
        Value _max;
        std::vector<boost::intrusive_ptr<Accumulator>> _accums;
    };

    /**
     * Consumes all of the documents from the source in the pipeline and sorts them by their
     * 'groupBy' value. This method might not be able to finish populating the sorter in a single
     * call if 'pSource' returns a DocumentSource::GetNextResult::kPauseExecution, so this returns
     * the last GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populateSorter();

    /**
     * Computes the 'groupBy' expression value for 'doc'.
     */
    Value extractKey(const Document& doc);

    /**
     * Calculates the bucket boundaries for the input documents and places them into buckets.
     */
    void populateBuckets();

    /**
     * Adds the document in 'entry' to 'bucket' by updating the accumulators in 'bucket'.
     */
    void addDocumentToBucket(const std::pair<Value, Document>& entry, Bucket& bucket);

    /**
     * Adds 'newBucket' to _buckets and updates any boundaries if necessary.
     */
    void addBucket(Bucket& newBucket);

    /**
     * Makes a document using the information from bucket. This is what is returned when getNext()
     * is called.
     */
    Document makeDocument(const Bucket& bucket);

    std::unique_ptr<Sorter<Value, Document>> _sorter;
    std::unique_ptr<Sorter<Value, Document>::Iterator> _sortedInput;

    // _fieldNames contains the field names for the result documents, _accumulatorFactories contains
    // the accumulator factories for the result documents, and _expressions contains the common
    // expressions used by each instance of each accumulator in order to find the right-hand side of
    // what gets added to the accumulator. These three vectors parallel each other.
    std::vector<std::string> _fieldNames;
    std::vector<Accumulator::Factory> _accumulatorFactories;
    std::vector<boost::intrusive_ptr<Expression>> _expressions;

    int _nBuckets;
    uint64_t _maxMemoryUsageBytes;
    bool _populated = false;
    std::vector<Bucket> _buckets;
    std::vector<Bucket>::iterator _bucketsIterator;
    std::unique_ptr<Variables> _variables;
    boost::intrusive_ptr<Expression> _groupByExpression;
    boost::intrusive_ptr<GranularityRounder> _granularityRounder;
    long long _nDocuments = 0;
};
}  // namespace mongo
