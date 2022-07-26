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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/generic_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

class Document;

/**
 * Registers a DocumentSource to have the name 'key'.
 *
 * 'liteParser' takes an AggregateCommandRequest and a BSONElement and returns a
 * LiteParsedDocumentSource. This is used for checks that need to happen before a full parse,
 * such as checks about which namespaces are referenced by this aggregation.
 *
 * 'fullParser' is either a DocumentSource::SimpleParser or a DocumentSource::Parser.
 * In both cases, it takes a BSONElement and an ExpressionContext and returns fully-executable
 * DocumentSource(s), for optimization and execution. In the common case it's a SimpleParser,
 * which returns a single DocumentSource; in the general case it's a Parser, which returns a whole
 * std::list to support "multi-stage aliases" like $bucket.
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
 */
#define REGISTER_DOCUMENT_SOURCE(key, liteParser, fullParser, allowedWithApiStrict) \
    REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(key,                                     \
                                           liteParser,                              \
                                           fullParser,                              \
                                           allowedWithApiStrict,                    \
                                           AllowedWithClientType::kAny,             \
                                           boost::none,                             \
                                           true)

/**
 * Like REGISTER_DOCUMENT_SOURCE, except the parser will only be enabled when FCV >= minVersion.
 * We store minVersion in the parserMap, so that changing FCV at runtime correctly enables/disables
 * the parser.
 */
#define REGISTER_DOCUMENT_SOURCE_WITH_MIN_VERSION(                      \
    key, liteParser, fullParser, allowedWithApiStrict, minVersion)      \
    REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(key,                         \
                                           liteParser,                  \
                                           fullParser,                  \
                                           allowedWithApiStrict,        \
                                           AllowedWithClientType::kAny, \
                                           minVersion,                  \
                                           true)

/**
 * Registers a DocumentSource which cannot be exposed to the users.
 */
#define REGISTER_INTERNAL_DOCUMENT_SOURCE(key, liteParser, fullParser, condition) \
    REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(key,                                   \
                                           liteParser,                            \
                                           fullParser,                            \
                                           AllowedWithApiStrict::kInternal,       \
                                           AllowedWithClientType::kInternal,      \
                                           boost::none,                           \
                                           condition)

/**
 * Like REGISTER_DOCUMENT_SOURCE_WITH_MIN_VERSION, except you can also specify a condition,
 * evaluated during startup, that decides whether to register the parser.
 *
 * For example, you could check a feature flag, and register the parser only when it's enabled.
 *
 * Note that the condition is evaluated only once, during a MONGO_INITIALIZER. Don't specify
 * a condition that can change at runtime, such as FCV. (Feature flags are ok, because they
 * cannot be toggled at runtime.)
 *
 * This is the most general REGISTER_DOCUMENT_SOURCE* macro, which all others should delegate to.
 */
#define REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(                                                  \
    key, liteParser, fullParser, allowedWithApiStrict, clientType, minVersion, ...)              \
    MONGO_INITIALIZER_GENERAL(addToDocSourceParserMap_##key,                                     \
                              ("BeginDocumentSourceRegistration"),                               \
                              ("EndDocumentSourceRegistration"))                                 \
    (InitializerContext*) {                                                                      \
        if (!__VA_ARGS__) {                                                                      \
            DocumentSource::registerParser("$" #key, DocumentSource::parseDisabled, minVersion); \
            LiteParsedDocumentSource::registerParser("$" #key,                                   \
                                                     LiteParsedDocumentSource::parseDisabled,    \
                                                     allowedWithApiStrict,                       \
                                                     clientType);                                \
            return;                                                                              \
        }                                                                                        \
        LiteParsedDocumentSource::registerParser(                                                \
            "$" #key, liteParser, allowedWithApiStrict, clientType);                             \
        DocumentSource::registerParser("$" #key, fullParser, minVersion);                        \
    }

/**
 * Like REGISTER_DOCUMENT_SOURCE, except the parser is only enabled when test-commands are enabled.
 */
#define REGISTER_TEST_DOCUMENT_SOURCE(key, liteParser, fullParser)                 \
    REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(key,                                    \
                                           liteParser,                             \
                                           fullParser,                             \
                                           AllowedWithApiStrict::kNeverInVersion1, \
                                           AllowedWithClientType::kAny,            \
                                           boost::none,                            \
                                           ::mongo::getTestCommandsEnabled())

class DocumentSource : public RefCountable {
public:
    // In general a parser returns a list of DocumentSources, to accomodate "multi-stage aliases"
    // like $bucket.
    using Parser = std::function<std::list<boost::intrusive_ptr<DocumentSource>>(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&)>;
    // But in the common case a parser returns only one DocumentSource.
    using SimpleParser = std::function<boost::intrusive_ptr<DocumentSource>(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&)>;

    using ChangeStreamRequirement = StageConstraints::ChangeStreamRequirement;
    using HostTypeRequirement = StageConstraints::HostTypeRequirement;
    using PositionRequirement = StageConstraints::PositionRequirement;
    using DiskUseRequirement = StageConstraints::DiskUseRequirement;
    using FacetRequirement = StageConstraints::FacetRequirement;
    using StreamType = StageConstraints::StreamType;
    using TransactionRequirement = StageConstraints::TransactionRequirement;
    using LookupRequirement = StageConstraints::LookupRequirement;
    using UnionRequirement = StageConstraints::UnionRequirement;

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

    /**
     * A struct representing the information needed to execute this stage on a distributed
     * collection. Describes how a pipeline should be split for sharded execution.
     */
    struct DistributedPlanLogic {
        DistributedPlanLogic() = default;

        /**
         * Convenience constructor for the common case where there is at most one merging stage. Can
         * pass nullptr for the merging stage which means "no merging required."
         */
        DistributedPlanLogic(boost::intrusive_ptr<DocumentSource> shardsStageIn,
                             boost::intrusive_ptr<DocumentSource> mergeStage,
                             boost::optional<BSONObj> mergeSortPatternIn = boost::none)
            : shardsStage(std::move(shardsStageIn)),
              mergeSortPattern(std::move(mergeSortPatternIn)) {
            if (mergeStage)
                mergingStages.emplace_back(std::move(mergeStage));
        }

        typedef std::function<bool(const DocumentSource&)> movePastFunctionType;
        // A stage which executes on each shard in parallel, or nullptr if nothing can be done in
        // parallel. For example, a partial $group before a subsequent global $group.
        boost::intrusive_ptr<DocumentSource> shardsStage = nullptr;

        // A stage or stages which funciton to merge all the results together, or an empty list if
        // nothing is necessary after merging. For example, a $limit stage.
        std::list<boost::intrusive_ptr<DocumentSource>> mergingStages = {};

        // If set, each document is expected to have sort key metadata which will be serialized in
        // the '$sortKey' field. 'mergeSortPattern' will then be used to describe which fields are
        // ascending and which fields are descending when merging the streams together.
        boost::optional<BSONObj> mergeSortPattern = boost::none;

        // If mergeSortPattern is specified and needsSplit is false, the split point will be
        // deferred to the next stage that would split the pipeline. The sortPattern will be taken
        // into account at that split point. Should be true if a stage specifies 'shardsStage' or
        // 'mergingStage'. Does not mean anything if the sort pattern is not set.
        bool needsSplit = true;

        // If needsSplit is false and this plan has anything that must run on the merging half of
        // the pipeline, it will be deferred until the next stage that sets any non-default value on
        // 'DistributedPlanLogic' or until a following stage causes the given validation
        // function to return false. By default this will not allow swapping with any
        // following stages.
        movePastFunctionType canMovePast = [](const DocumentSource&) { return false; };
    };

    virtual ~DocumentSource() {}

    /**
     * Makes a deep clone of the DocumentSource by serializing and re-parsing it. DocumentSources
     * that cannot be safely cloned this way should override this method. Callers can optionally
     * specify 'newExpCtx' to construct the deep clone with it instead of defaulting to the
     * original's 'ExpressionContext'.
     */
    virtual boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx = nullptr) const {
        auto expCtx = newExpCtx ? newExpCtx : pExpCtx;
        std::vector<Value> serializedDoc;
        serializeToArray(serializedDoc);
        tassert(5757900,
                str::stream() << "DocumentSource " << getSourceName()
                              << " should have serialized to exactly one document. This stage may "
                                 "need a custom clone() implementation",
                serializedDoc.size() == 1 && serializedDoc[0].getType() == BSONType::Object);
        auto dsList = parse(expCtx, Document(serializedDoc[0].getDocument()).toBson());
        // Cloning should only happen once the pipeline has been fully built, after desugaring from
        // one stage to multiple stages has occurred. When cloning desugared stages we expect each
        // stage to re-parse to one stage.
        tassert(5757901,
                str::stream() << "DocumentSource " << getSourceName()
                              << " parse should have returned single document. This stage may need "
                                 "a custom clone() implementation",
                dsList.size() == 1);
        return dsList.front();
    }

    /**
     * The main execution API of a DocumentSource. Returns an intermediate query result generated by
     * this DocumentSource.
     *
     * For performance reasons, a streaming stage must not keep references to documents across calls
     * to getNext(). Such stages must retrieve a result from their child and then release it (or
     * return it) before asking for another result. Failing to do so can result in extra work, since
     * the Document/Value library must copy data on write when that data has a refcount above one.
     */
    GetNextResult getNext() {
        pExpCtx->checkForInterrupt();

        if (MONGO_likely(!pExpCtx->shouldCollectDocumentSourceExecStats())) {
            return doGetNext();
        }

        auto serviceCtx = pExpCtx->opCtx->getServiceContext();
        invariant(serviceCtx);
        auto fcs = serviceCtx->getFastClockSource();
        invariant(fcs);

        invariant(_commonStats.executionTimeMillis);
        ScopedTimer timer(fcs, _commonStats.executionTimeMillis.get_ptr());
        ++_commonStats.works;

        GetNextResult next = doGetNext();
        if (next.isAdvanced()) {
            ++_commonStats.advanced;
        }
        return next;
    }

    /**
     * Returns a struct containing information about any special constraints imposed on using this
     * stage. Input parameter Pipeline::SplitState is used by stages whose requirements change
     * depending on whether they are in a split or unsplit pipeline.
     */
    virtual StageConstraints constraints(
        Pipeline::SplitState = Pipeline::SplitState::kUnsplit) const = 0;

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
     * Get the CommonStats for this DocumentSource.
     */
    const CommonStats& getCommonStats() const {
        return _commonStats;
    }

    /**
     * Get the stats specific to the DocumentSource. It is legal for the DocumentSource to return
     * nullptr to indicate that no specific stats are available.
     */
    virtual const SpecificStats* getSpecificStats() const {
        return nullptr;
    }

    /**
     * Get the stage's name.
     */
    virtual const char* getSourceName() const = 0;

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
     * Shortcut method to get a BSONObj for debugging. Often useful in log messages, but is not
     * cheap so avoid doing so on a hot path at a low verbosity.
     */
    virtual BSONObj serializeToBSONForDebug() const;

    /**
     * If this stage uses additional namespaces, adds them to 'collectionNames'. These namespaces
     * should all be names of collections, not views.
     */
    virtual void addInvolvedCollections(
        stdx::unordered_set<NamespaceString>* collectionNames) const {}

    virtual void detachFromOperationContext() {}

    virtual void reattachToOperationContext(OperationContext* opCtx) {}

    virtual bool usedDisk() {
        return false;
    };

    /**
     * Create a DocumentSource pipeline stage from 'stageObj'.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONObj stageObj);

    /**
     * Function that will be used as an alternate parser for a document source that has been
     * disabled.
     */
    static boost::intrusive_ptr<DocumentSource> parseDisabled(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        uasserted(
            ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << elem.fieldName()
                          << " is not allowed with the current configuration. You may need to "
                             "enable the corresponding feature flag");
    }
    /**
     * Registers a DocumentSource with a parsing function, so that when a stage with the given name
     * is encountered, it will call 'parser' to construct that stage.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_DOCUMENT_SOURCE macro defined in
     * this file.
     */
    static void registerParser(
        std::string name,
        Parser parser,
        boost::optional<multiversion::FeatureCompatibilityVersion> requiredMinVersion);
    /**
     * Convenience wrapper for the common case, when DocumentSource::Parser returns a list of one
     * DocumentSource.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_DOCUMENT_SOURCE macro defined in
     * this file.
     */
    static void registerParser(
        std::string name,
        SimpleParser simpleParser,
        boost::optional<multiversion::FeatureCompatibilityVersion> requiredMinVersion);

    /**
     * Returns true if the DocumentSource has a query.
     */
    virtual bool hasQuery() const;

    /**
     * Returns the DocumentSource query if it exists.
     */
    virtual BSONObj getQuery() const;

private:
    /**
     * Attempt to push a match stage from directly ahead of the current stage given by itr to before
     * the current stage. Returns whether the optimization was performed.
     */
    bool pushMatchBefore(Pipeline::SourceContainer::iterator itr,
                         Pipeline::SourceContainer* container);

    /**
     * Attempt to push a sample stage from directly ahead of the current stage given by itr to
     * before the current stage. Returns whether the optimization was performed.
     */
    bool pushSampleBefore(Pipeline::SourceContainer::iterator itr,
                          Pipeline::SourceContainer* container);

    /**
     * Attempts to push any kind of 'DocumentSourceSingleDocumentTransformation' stage directly
     * ahead of the stage present at the 'itr' position if matches the constraints. Returns true if
     * optimization was performed, false otherwise.
     *
     * Note that this optimization is oblivious to the transform function. The only stages that are
     * eligible to swap are those that can safely swap with any transform.
     */
    bool pushSingleDocumentTransformBefore(Pipeline::SourceContainer::iterator itr,
                                           Pipeline::SourceContainer* container);

    /**
     * Wraps various optimization methods and returns the call immediately if any one of them
     * returns true.
     */
    bool attemptToPushStageBefore(Pipeline::SourceContainer::iterator itr,
                                  Pipeline::SourceContainer* container) {
        if (std::next(itr) == container->end()) {
            return false;
        }

        return pushMatchBefore(itr, container) || pushSampleBefore(itr, container) ||
            pushSingleDocumentTransformBefore(itr, container);
    }

public:
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

        GetModPathsReturn(Type type, OrderedPathSet&& paths, StringMap<std::string>&& renames)
            : type(type), paths(std::move(paths)), renames(std::move(renames)) {}

        std::set<std::string> getNewNames() {
            std::set<std::string> newNames;
            for (auto&& name : paths) {
                newNames.insert(name);
            }
            for (auto&& rename : renames) {
                newNames.insert(rename.first);
            }
            return newNames;
        }

        bool canModify(const FieldPath& fieldPath) const {
            switch (type) {
                case Type::kAllPaths:
                    return true;
                case Type::kNotSupported:
                    return true;
                case Type::kFiniteSet:
                    // If there's a subpath that is modified this path may be modified.
                    for (size_t i = 0; i < fieldPath.getPathLength(); i++) {
                        if (paths.count(fieldPath.getSubpath(i).toString()))
                            return true;
                    }

                    for (auto&& path : paths) {
                        // If there's a superpath that is modified this path may be modified.
                        if (expression::isPathPrefixOf(fieldPath.fullPath(), path)) {
                            return true;
                        }
                    }

                    return false;
                case Type::kAllExcept:
                    // If one of the subpaths is unmodified return false.
                    for (size_t i = 0; i < fieldPath.getPathLength(); i++) {
                        if (paths.count(fieldPath.getSubpath(i).toString()))
                            return false;
                    }

                    // Otherwise return true;
                    return true;
            }
            // Cannot hit.
            MONGO_UNREACHABLE_TASSERT(6434901);
        }

        Type type;
        OrderedPathSet paths;

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
        return {GetModPathsReturn::Type::kNotSupported, OrderedPathSet{}, {}};
    }

    /**
     * Returns the expression context from the stage's context.
     */
    const boost::intrusive_ptr<ExpressionContext>& getContext() const {
        return pExpCtx;
    }

    /**
     * Get the dependencies this operation needs to do its job. If overridden, subclasses must add
     * all paths needed to apply their transformation to 'deps->fields', and call
     * 'deps->setNeedsMetadata()' to indicate what metadata (e.g. text score), if any, is required.
     *
     * See DepsTracker::State for the possible return values and what they mean.
     */
    virtual DepsTracker::State getDependencies(DepsTracker* deps) const {
        return DepsTracker::State::NOT_SUPPORTED;
    }

    /**
     * If this stage can be run in parallel across a distributed collection, returns boost::none.
     * Otherwise, returns a struct representing what needs to be done to merge each shard's pipeline
     * into a single stream of results. Must not mutate the existing source object; if different
     * behaviour is required, a new source should be created and configured appropriately. It is an
     * error for the returned DistributedPlanLogic to have identical pointers for 'shardsStage' and
     * 'mergingStage'.
     */
    virtual boost::optional<DistributedPlanLogic> distributedPlanLogic() = 0;

    /**
     * Returns true if it would be correct to execute this stage in parallel across the shards in
     * cases where the final stage is a stage which can perform a write operation, such as $merge.
     * For example, a $group stage which is just merging the groups from the shards can be run in
     * parallel since it will preserve the shard key.
     */
    virtual bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const {
        return false;
    }

    /**
     * For stages that have sub-pipelines returns the source container holding the stages of that
     * pipeline. Otherwise returns a nullptr.
     */
    virtual const Pipeline::SourceContainer* getSubPipeline() const {
        return nullptr;
    }

protected:
    DocumentSource(StringData stageName, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * The main execution API of a DocumentSource. Returns an intermediate query result generated by
     * this DocumentSource. See comment at getNext().
     */
    virtual GetNextResult doGetNext() = 0;

    /**
     * Attempt to perform an optimization with the following source in the pipeline. 'container'
     * refers to the entire pipeline, and 'itr' points to this stage within the pipeline.
     *
     * The return value is an iterator over the same container which points to the first location
     * in the container at which an optimization may be possible, or the end of the container().
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
    CommonStats _commonStats;

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

/**
 * Method to accumulate the plan summary stats from all stages of the pipeline into the given
 * `planSummaryStats` object.
 */
void accumulatePipelinePlanSummaryStats(const Pipeline& pipeline,
                                        PlanSummaryStats& planSummaryStats);

}  // namespace mongo
