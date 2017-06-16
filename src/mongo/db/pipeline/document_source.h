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

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

class AggregationRequest;
class Document;

/**
 * Registers a DocumentSource to have the name 'key'.
 *
 * 'liteParser' takes an AggregationRequest and a BSONElement and returns a
 * LiteParsedDocumentSource. This is used for checks that need to happen before a full parse,
 * such as checks about which namespaces are referenced by this aggregation.
 *
 * 'fullParser' takes a BSONElement and an ExpressionContext and returns a fully-executable
 * DocumentSource. This will be used for optimization and execution.
 *
 * Stages that do not require any special pre-parse checks can use
 * LiteParsedDocumentSourceDefault::parse as their 'liteParser'.
 *
 * As an example, if your stage DocumentSourceFoo looks like {$foo: <args>} and does *not* require
 * any special pre-parse checks, you should implement a static parser like
 * DocumentSourceFoo::createFromBson(), and register it like so:
 * REGISTER_DOCUMENT_SOURCE(foo,
 *                          LiteParsedDocumentSourceDefault::parse,
 *                          DocumentSourceFoo::createFromBson);
 *
 * If your stage is actually an alias which needs to return more than one stage (such as
 * $sortByCount), you should use the REGISTER_MULTI_STAGE_ALIAS macro instead.
 */
#define REGISTER_DOCUMENT_SOURCE(key, liteParser, fullParser)                                \
    MONGO_INITIALIZER(addToDocSourceParserMap_##key)(InitializerContext*) {                  \
        auto fullParserWrapper = [](BSONElement stageSpec,                                   \
                                    const boost::intrusive_ptr<ExpressionContext>& expCtx) { \
            return std::vector<boost::intrusive_ptr<DocumentSource>>{                        \
                (fullParser)(stageSpec, expCtx)};                                            \
        };                                                                                   \
        LiteParsedDocumentSource::registerParser("$" #key, liteParser);                      \
        DocumentSource::registerParser("$" #key, fullParserWrapper);                         \
        return Status::OK();                                                                 \
    }

/**
 * Registers a multi-stage alias (such as $sortByCount) to have the single name 'key'. When a stage
 * with name '$key' is found, 'liteParser' will be used to produce a LiteParsedDocumentSource,
 * while 'fullParser' will be called to construct a vector of DocumentSources. See the comments on
 * REGISTER_DOCUMENT_SOURCE for more information.
 *
 * As an example, if your stage alias looks like {$foo: <args>} and does *not* require any special
 * pre-parse checks, you should implement a static parser like DocumentSourceFoo::createFromBson(),
 * and register it like so:
 * REGISTER_MULTI_STAGE_ALIAS(foo,
 *                            LiteParsedDocumentSourceDefault::parse,
 *                            DocumentSourceFoo::createFromBson);
 */
#define REGISTER_MULTI_STAGE_ALIAS(key, liteParser, fullParser)                  \
    MONGO_INITIALIZER(addAliasToDocSourceParserMap_##key)(InitializerContext*) { \
        LiteParsedDocumentSource::registerParser("$" #key, (liteParser));        \
        DocumentSource::registerParser("$" #key, (fullParser));                  \
        return Status::OK();                                                     \
    }

class DocumentSource : public IntrusiveCounterUnsigned {
public:
    using Parser = stdx::function<std::vector<boost::intrusive_ptr<DocumentSource>>(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&)>;

    enum class InitialSourceType {
        // Stage requires input from a preceding DocumentSource.
        kNotInitialSource,
        // Stage does not need an input source and should be the first stage in the pipeline.
        kInitialSource,
        // Similar to kInitialSource, but does not require an underlying collection to produce
        // output.
        kCollectionlessInitialSource
    };

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
     *
     * For performance reasons, a streaming stage must not keep references to documents across calls
     * to getNext(). Such stages must retrieve a result from their child and then release it (or
     * return it) before asking for another result. Failing to do so can result in extra work, since
     * the Document/Value library must copy data on write when that data has a refcount above one.
     */
    virtual GetNextResult getNext() = 0;

    /**
     * Informs the stage that it is no longer needed and can release its resources. After dispose()
     * is called the stage must still be able to handle calls to getNext(), but can return kEOF.
     *
     * This is a non-virtual public interface to ensure dispose() is threaded through the entire
     * pipeline. Subclasses should override doDispose() to implement their disposal.
     */
    void dispose() {
        doDispose();
        if (pSource) {
            pSource->dispose();
        }
    }

    /**
     * Get the stage's name.
     */
    virtual const char* getSourceName() const;

    /**
     * Set the underlying source this source should use to get Documents from. Must not throw
     * exceptions.
     */
    virtual void setSource(DocumentSource* source) {
        pSource = source;
    }

    /**
     * In the default case, serializes the DocumentSource and adds it to the std::vector<Value>.
     *
     * A subclass may choose to overwrite this, rather than serialize, if it should output multiple
     * stages (eg, $sort sometimes also outputs a $limit).
     *
     * The 'explain' parameter indicates the explain verbosity mode, or is equal boost::none if no
     * explain is requested.
     */
    virtual void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const;

    /**
     * Subclasses should return InitialSourceType::kInitialSource if the stage does not require an
     * input source, or InitialSourceType::kCollectionlessInitialSource if the stage will produce
     * the input for the pipeline independent of an underlying collection. The latter are specified
     * with {aggregate: 1}, e.g. $currentOp.
     */
    virtual InitialSourceType getInitialSourceType() const {
        return InitialSourceType::kNotInitialSource;
    }

    /**
     * Returns true if this stage does not require an input source.
     */
    bool isInitialSource() const {
        return getInitialSourceType() != InitialSourceType::kNotInitialSource;
    }

    /**
     * Returns true if this stage will produce the input for the pipeline independent of an
     * underlying collection. These are specified with {aggregate: 1}, e.g. $currentOp.
     */
    bool isCollectionlessInitialSource() const {
        return getInitialSourceType() == InitialSourceType::kCollectionlessInitialSource;
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
     * Create a DocumentSource pipeline stage from 'stageObj'.
     */
    static std::vector<boost::intrusive_ptr<DocumentSource>> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONObj stageObj);

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

    //
    // Optimization API - These methods give each DocumentSource an opportunity to apply any local
    // optimizations, and to provide any rule-based optimizations to swap with or absorb subsequent
    // stages.
    //

    /**
     * The non-virtual public interface for optimization. Attempts to do some generic optimizations
     * such as pushing $matches as early in the pipeline as possible, then calls out to
     * doOptimizeAt() for stage-specific optimizations.
     *
     * Subclasses should override doOptimizeAt() if they can apply some optimization(s) based on
     * subsequent stages in the pipeline.
     */
    Pipeline::SourceContainer::iterator optimizeAt(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container);

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

    //
    // Property Analysis - These methods allow a DocumentSource to expose information about
    // properties of themselves, such as which fields they need to apply their transformations, and
    // whether or not they produce or preserve a sort order.
    //
    // Property analysis can be useful during optimization (e.g. analysis of sort orders determines
    // whether or not a blocking group can be upgraded to a streaming group).
    //

    /**
     * Gets a BSONObjSet representing the sort order(s) of the output of the stage.
     */
    virtual BSONObjSet getOutputSorts() {
        return SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    struct GetModPathsReturn {
        enum class Type {
            // No information is available about which paths are modified.
            kNotSupported,

            // All fields will be modified. This should be used by stages like $replaceRoot which
            // modify the entire document.
            kAllPaths,

            // A finite set of paths will be modified by this stage. This is true for something like
            // {$project: {a: 0, b: 0}}, which will only modify 'a' and 'b', and leave all other
            // paths unmodified.
            kFiniteSet,

            // This stage will modify an infinite set of paths, but we know which paths it will not
            // modify. For example, the stage {$project: {_id: 1, a: 1}} will leave only the fields
            // '_id' and 'a' unmodified, but all other fields will be projected out.
            kAllExcept,
        };

        GetModPathsReturn(Type type,
                          std::set<std::string>&& paths,
                          StringMap<std::string>&& renames)
            : type(type), paths(std::move(paths)), renames(std::move(renames)) {}

        Type type;
        std::set<std::string> paths;

        // Stages may fill out 'renames' to contain information about path renames. Each entry in
        // 'renames' maps from the new name of the path (valid in documents flowing *out* of this
        // stage) to the old name of the path (valid in documents flowing *into* this stage).
        //
        // For example, consider the stage
        //
        //   {$project: {_id: 0, a: 1, b: "$c"}}
        //
        // This stage should return kAllExcept, since it modifies all paths other than "a". It can
        // also fill out 'renames' with the mapping "b" => "c".
        StringMap<std::string> renames;
    };

    /**
     * Returns information about which paths are added, removed, or updated by this stage. The
     * default implementation uses kNotSupported to indicate that the set of modified paths for this
     * stage is not known.
     *
     * See GetModPathsReturn above for the possible return values and what they mean.
     */
    virtual GetModPathsReturn getModifiedPaths() const {
        return {GetModPathsReturn::Type::kNotSupported, std::set<std::string>{}, {}};
    }

    /**
     * Returns whether this stage can swap with a subsequent $match stage, provided that the match
     * does not depend on the paths returned by getModifiedPaths().
     *
     * Subclasses which want to participate in match swapping should override this to return true.
     * Such a subclass must also override getModifiedPaths() to provide information about which
     * $match predicates be swapped before itself.
     */
    virtual bool canSwapWithMatch() const {
        return false;
    }

    enum GetDepsReturn {
        // The full object and all metadata may be required.
        NOT_SUPPORTED = 0x0,

        // Later stages could need either fields or metadata. For example, a $limit stage will pass
        // through all fields, and they may or may not be needed by future stages.
        SEE_NEXT = 0x1,

        // Later stages won't need more fields from input. For example, an inclusion projection like
        // {_id: 1, a: 1} will only output two fields, so future stages cannot possibly depend on
        // any other fields.
        EXHAUSTIVE_FIELDS = 0x2,

        // Later stages won't need more metadata from input. For example, a $group stage will group
        // documents together, discarding their text score.
        EXHAUSTIVE_META = 0x4,

        // Later stages won't need either fields or metadata.
        EXHAUSTIVE_ALL = EXHAUSTIVE_FIELDS | EXHAUSTIVE_META,
    };

    /**
     * Get the dependencies this operation needs to do its job. If overridden, subclasses must add
     * all paths needed to apply their transformation to 'deps->fields', and call
     * 'deps->setNeedTextScore()' if the text score is required.
     *
     * See GetDepsReturn above for the possible return values and what they mean.
     */
    virtual GetDepsReturn getDependencies(DepsTracker* deps) const {
        return NOT_SUPPORTED;
    }

protected:
    explicit DocumentSource(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Attempt to perform an optimization with the following source in the pipeline. 'container'
     * refers to the entire pipeline, and 'itr' points to this stage within the pipeline. The caller
     * must guarantee that std::next(itr) != container->end().
     *
     * The return value is an iterator over the same container which points to the first location
     * in the container at which an optimization may be possible.
     *
     * For example, if a swap takes place, the returned iterator should just be the position
     * directly preceding 'itr', if such a position exists, since the stage at that position may be
     * able to perform further optimizations with its new neighbor.
     */
    virtual Pipeline::SourceContainer::iterator doOptimizeAt(
        Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
        return std::next(itr);
    };

    /**
     * Release any resources held by this stage. After doDispose() is called the stage must still be
     * able to handle calls to getNext(), but can return kEOF.
     */
    virtual void doDispose() {}

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
     *
     * The 'explain' parameter indicates the explain verbosity mode, or is equal boost::none if no
     * explain is requested.
     */
    virtual Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const = 0;
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
        enum class CurrentOpConnectionsMode { kIncludeIdle, kExcludeIdle };
        enum class CurrentOpUserMode { kIncludeAll, kExcludeOthers };

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
         * Appends the record count for collection "nss" to "builder".
         */
        virtual Status appendRecordCount(const NamespaceString& nss,
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
        virtual StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> makePipeline(
            const std::vector<BSONObj>& rawPipeline,
            const boost::intrusive_ptr<ExpressionContext>& expCtx) = 0;

        /**
         * Returns a vector of owned BSONObjs, each of which contains details of an in-progress
         * operation or, optionally, an idle connection. If userMode is kIncludeAllUsers, report
         * operations for all authenticated users; otherwise, report only the current user's
         * operations.
         */
        virtual std::vector<BSONObj> getCurrentOps(CurrentOpConnectionsMode connMode,
                                                   CurrentOpUserMode userMode) const = 0;

        /**
         * Returns the name of the local shard if sharding is enabled, or an empty string.
         */
        virtual std::string getShardName(OperationContext* opCtx) const = 0;

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


}  // namespace mongo
