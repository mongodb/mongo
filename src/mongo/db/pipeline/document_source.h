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
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

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
                                           nullptr, /* featureFlag */               \
                                           true)

/**
 * Like REGISTER_DOCUMENT_SOURCE, except the parser will only be registered when featureFlag is
 * enabled. We store featureFlag in the parserMap, so that it can be checked at runtime
 * to correctly enable/disable the parser.
 */
#define REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(                     \
    key, liteParser, fullParser, allowedWithApiStrict, featureFlag)     \
    REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(key,                         \
                                           liteParser,                  \
                                           fullParser,                  \
                                           allowedWithApiStrict,        \
                                           AllowedWithClientType::kAny, \
                                           featureFlag,                 \
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
                                           nullptr, /* featureFlag*/              \
                                           condition)

/**
 * You can specify a condition, evaluated during startup,
 * that decides whether to register the parser.
 *
 * For example, you could check a feature flag, and register the parser only when it's enabled.
 *
 * Note that the condition is evaluated only once, during a MONGO_INITIALIZER. Don't specify
 * a condition that can change at runtime, such as FCV. (Feature flags are ok, because they
 * cannot be toggled at runtime.)
 *
 * This is the most general REGISTER_DOCUMENT_SOURCE* macro, which all others should delegate to.
 */
#define REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(                                                   \
    key, liteParser, fullParser, allowedWithApiStrict, clientType, featureFlag, ...)              \
    MONGO_INITIALIZER_GENERAL(addToDocSourceParserMap_##key,                                      \
                              ("BeginDocumentSourceRegistration"),                                \
                              ("EndDocumentSourceRegistration"))                                  \
    (InitializerContext*) {                                                                       \
        /* Require 'featureFlag' to be a constexpr. */                                            \
        constexpr FeatureFlag* constFeatureFlag{featureFlag};                                     \
        /* This non-constexpr variable works around a bug in GCC when 'featureFlag' is null. */   \
        FeatureFlag* featureFlagValue{constFeatureFlag};                                          \
        bool evaluatedCondition{__VA_ARGS__};                                                     \
        if (!evaluatedCondition || (featureFlagValue && !featureFlagValue->canBeEnabled())) {     \
            DocumentSource::registerParser("$" #key, DocumentSource::parseDisabled, featureFlag); \
            LiteParsedDocumentSource::registerParser("$" #key,                                    \
                                                     LiteParsedDocumentSource::parseDisabled,     \
                                                     allowedWithApiStrict,                        \
                                                     clientType);                                 \
            return;                                                                               \
        }                                                                                         \
        LiteParsedDocumentSource::registerParser(                                                 \
            "$" #key, liteParser, allowedWithApiStrict, clientType);                              \
        DocumentSource::registerParser("$" #key, fullParser, featureFlag);                        \
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
                                           nullptr, /* featureFlag */              \
                                           ::mongo::getTestCommandsEnabled())

/**
 * Allocates a new, unique DocumentSource::Id value.
 * Assigns it to a private variable (in an anonymous namespace) based on the given `name`, and
 * declares a const reference named `constName` to the private variable.
 */
#define ALLOCATE_DOCUMENT_SOURCE_ID(name, constName)                  \
    namespace {                                                       \
    DocumentSource::Id _dsid_##name = DocumentSource::kUnallocatedId; \
    MONGO_INITIALIZER_GENERAL(allocateDocSourceId_##name,             \
                              ("BeginDocumentSourceIdAllocation"),    \
                              ("EndDocumentSourceIdAllocation"))      \
    (InitializerContext*) {                                           \
        _dsid_##name = DocumentSource::allocateId(#name);             \
    }                                                                 \
    }                                                                 \
    const DocumentSource::Id& constName = _dsid_##name;

class DocumentSource;
using DocumentSourceContainer = std::list<boost::intrusive_ptr<DocumentSource>>;

class Pipeline;

namespace exec::agg {
class ListMqlEntitiesStage;
}  // namespace exec::agg

// TODO SPM-4106: Remove virtual keyword once the refactoring is done.
class DocumentSource : public virtual RefCountable {
public:
    // In general a parser returns a list of DocumentSources, to accommodate "multi-stage aliases"
    // like $bucket.
    using Parser = std::function<DocumentSourceContainer(
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
    using GetNextResult = exec::agg::GetNextResult;

    /**
     * Used to identify different DocumentSource sub-classes, without requiring RTTI.
     */
    using Id = unsigned long;

    // Using 0 for "unallocated id" makes it easy to check if an Id has been allocated.
    static constexpr Id kUnallocatedId{0};

    struct ParserRegistration {
        DocumentSource::Parser parser;
        FeatureFlag* featureFlag;
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
            if (mergeStage) {
                mergingStages.emplace_back(std::move(mergeStage));
            }
        }

        typedef std::function<bool(const DocumentSource&)> movePastFunctionType;
        // A stage which executes on each shard in parallel, or nullptr if nothing can be done in
        // parallel. For example, a partial $group before a subsequent global $group.
        boost::intrusive_ptr<DocumentSource> shardsStage = nullptr;

        // A stage or stages which function to merge all the results together, or an empty list if
        // nothing is necessary after merging. For example, a $limit stage.
        DocumentSourceContainer mergingStages = {};

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
        movePastFunctionType canMovePast = [](const DocumentSource&) {
            return false;
        };
    };

    /**
     * Describes context pipelineDependentDistributedPlanLogic() should take into account to make a
     * decision about whether or not a document source can be pushed down to shards.
     */
    struct DistributedPlanContext {
        // The pipeline up to but not including the current document source.
        const Pipeline& pipelinePrefix;
        // The pipeline after and not including the document source.
        const Pipeline& pipelineSuffix;
        // The set of paths provided by the shard key.
        const boost::optional<OrderedPathSet>& shardKeyPaths;
    };

    ~DocumentSource() override {}

    /**
     * Makes a deep clone of the DocumentSource by serializing and re-parsing it. DocumentSources
     * that cannot be safely cloned this way should override this method. Callers can optionally
     * specify 'newExpCtx' to construct the deep clone with it instead of defaulting to the
     * original's 'ExpressionContext'.
     */
    virtual boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
        tassert(7406001, "expCtx passed to clone must not be null", expCtx);
        std::vector<Value> serializedDoc;
        serializeToArray(serializedDoc, SerializationOptions{.serializeForCloning = true});
        tassert(5757900,
                str::stream() << "DocumentSource " << getSourceName()
                              << " should have serialized to exactly one document. This stage may "
                                 "need a custom clone() implementation",
                serializedDoc.size() == 1 && serializedDoc[0].getType() == BSONType::object);
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
     * Returns a struct containing information about any special constraints imposed on using this
     * stage. Input parameter PipelineSplitState is used by stages whose requirements change
     * depending on whether they are in a split or unsplit pipeline.
     */
    virtual StageConstraints constraints(
        PipelineSplitState = PipelineSplitState::kUnsplit) const = 0;

    /**
     * If a stage's StageConstraints::PositionRequirement is kCustom, then it should also override
     * this method, which will be called by the validation process.
     */
    virtual void validatePipelinePosition(bool alreadyOptimized,
                                          DocumentSourceContainer::const_iterator pos,
                                          const DocumentSourceContainer& container) const {
        MONGO_UNIMPLEMENTED_TASSERT(7183905);
    };

    /**
     * Get the stage's name.
     */
    virtual const char* getSourceName() const = 0;

    /**
     * Returns the DocumentSource::Id value of a given stage object.
     * Each child class should override this and return that class's static `id` value.
     */
    virtual Id getId() const = 0;

    /**
     * In the default case, serializes the DocumentSource and adds it to the std::vector<Value>.
     *
     * A subclass may choose to overwrite this, rather than serialize, if it should output multiple
     * stages (eg, $sort sometimes also outputs a $limit).
     */
    virtual void serializeToArray(std::vector<Value>& array,
                                  const SerializationOptions& opts = SerializationOptions{}) const;

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

    /**
     * Create a DocumentSource pipeline stage from 'stageObj'.
     */
    static DocumentSourceContainer parse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         BSONObj stageObj);

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
    static void registerParser(std::string name, Parser parser, FeatureFlag* featureFlag = nullptr);
    /**
     * Convenience wrapper for the common case, when DocumentSource::Parser returns a list of one
     * DocumentSource.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_DOCUMENT_SOURCE macro defined in
     * this file.
     */
    static void registerParser(std::string name,
                               SimpleParser simpleParser,
                               FeatureFlag* featureFlag = nullptr);

    /**
     * Allocate and return a new, unique DocumentSource::Id value.
     *
     * DO NOT call this method directly. Instead, use the ALLOCATE_DOCUMENT_SOURCE_ID macro defined
     * in this file.
     */
    static Id allocateId(StringData name);

    /**
     * Returns true if the DocumentSource has a query.
     */
    virtual bool hasQuery() const;

    /**
     * Returns the DocumentSource query if it exists.
     */
    virtual BSONObj getQuery() const;

    /**
     * Utility which allows for accessing and computing a ShardId to act as a merger.
     */
    boost::optional<ShardId> getMergeShardId() const {
        return mergeShardId.get();
    }

private:
    /**
     * itr is pointing to some stage `A`. Fetch stage `B`, the stage after A in itr. If B is a
     * $match stage, attempt to push B before A. Returns whether this optimization was
     * performed.
     */
    bool pushMatchBefore(DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);

    /**
     * itr is pointing to some stage `A`. Fetch stage `B`, the stage after A in itr. If B is a
     * $sample stage, attempt to push B before A. Returns whether this optimization was
     * performed.
     */
    bool pushSampleBefore(DocumentSourceContainer::iterator itr,
                          DocumentSourceContainer* container);

    /**
     * Attempts to push any kind of 'DocumentSourceSingleDocumentTransformation' stage or a $redact
     * stage directly ahead of the stage present at the 'itr' position if matches the constraints.
     * Returns true if optimization was performed, false otherwise.
     *
     * Note that this optimization is oblivious to the transform function. The only stages that are
     * eligible to swap are those that can safely swap with any transform.
     */
    bool pushSingleDocumentTransformOrRedactBefore(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container);

    /**
     * Wraps various optimization methods and returns the call immediately if any one of them
     * returns true.
     */
    bool attemptToPushStageBefore(DocumentSourceContainer::iterator itr,
                                  DocumentSourceContainer* container) {
        if (std::next(itr) == container->end()) {
            return false;
        }

        return pushMatchBefore(itr, container) || pushSampleBefore(itr, container) ||
            pushSingleDocumentTransformOrRedactBefore(itr, container);
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
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

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
        // Note that renaming a path does NOT count as a modification (see `renames` struct member).
        // Modification can be, in the context of the query: the removal of a path, the creation of
        // a new path, etc.
        enum class Type {
            // No information is available about which paths are modified.
            kNotSupported,

            // All fields will be modified. This should be used by stages like $replaceRoot which
            // modify the entire document.
            kAllPaths,

            // A finite set of paths will be modified by this stage. This is true for something like
            // {$project: {a: 0, b: 0}}, which will only modify 'a' and 'b', and leave all other
            // paths unmodified. Other examples include: $lookup, $unwind.
            kFiniteSet,

            // This stage will modify an infinite set of paths, but we know which paths it will not
            // modify. For example, the stage {$project: {_id: 1, a: 1}} will leave only the fields
            // '_id' and 'a' unmodified, but all other fields will be projected out. Other examples
            // include: $group.
            kAllExcept,
        };

        GetModPathsReturn(Type type,
                          OrderedPathSet&& paths,
                          StringMap<std::string>&& renames,
                          StringMap<std::string>&& complexRenames = {})
            : type(type),
              paths(std::move(paths)),
              renames(std::move(renames)),
              complexRenames(std::move(complexRenames)) {}

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
                        if (paths.count(std::string{fieldPath.getSubpath(i)}))
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
                        if (paths.count(std::string{fieldPath.getSubpath(i)}))
                            return false;
                    }

                    // Otherwise return true;
                    return true;
            }
            // Cannot hit.
            MONGO_UNREACHABLE_TASSERT(6434902);
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

        // Including space for returning renames which include dotted paths.
        // i.e., "a.b" => c
        //
        StringMap<std::string> complexRenames;
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
     * TODO SPM-4106: Consider renaming to getContext() once the refactoring is done.
     */
    const boost::intrusive_ptr<ExpressionContext>& getExpCtx() const {
        return _expCtx;
    }

    /**
     * Get the dependencies this operation needs to do its job. If overridden, subclasses must add
     * all paths needed to apply their transformation to 'deps->fields', and call
     * 'deps->setNeedsMetadata()' to indicate what metadata (e.g. text score), if any, is required.
     *
     * getDependencies() is also used to implement validation / error reporting for $meta
     * dependencies. There may be some incomplete implementations of getDependencies() that return
     * NOT_SUPPORTED even though they call to 'deps->setMetadataAvailable()'. This is because
     * they've been implemented correctly for error reporting but not for dependency analysis.
     * TODO SERVER-100902 Split $meta validation separate from dependency analysis.
     *
     * See DepsTracker::State for the possible return values and what they mean.
     */
    virtual DepsTracker::State getDependencies(DepsTracker* deps) const {
        return DepsTracker::State::NOT_SUPPORTED;
    }

    /**
     * Populate 'refs' with the variables referred to by this stage, including user and system
     * variables but excluding $$ROOT. Note that field path references are not considered variables.
     */
    virtual void addVariableRefs(std::set<Variables::Id>* refs) const = 0;

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
     * Check if a source is able to run in parallel across a distributed collection, given
     * sharding context and that it will be evaluated _after_ pipelinePrefix, and _before_
     * pipelineSuffix.
     *
     * For stages which do not have any pipeline-dependent behaviour, the default impl
     * calls distributedPlanLogic.
     *
     * See distributedPlanLogic() for conditions and return values.
     */
    virtual boost::optional<DistributedPlanLogic> pipelineDependentDistributedPlanLogic(
        const DistributedPlanContext& ctx) {
        return distributedPlanLogic();
    }

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
    virtual const DocumentSourceContainer* getSubPipeline() const {
        return nullptr;
    }

    virtual void detachSourceFromOperationContext() {}

    virtual void reattachSourceToOperationContext(OperationContext* opCtx) {}

    /**
     * Validate that all operation contexts associated with this document source, including any
     * subpipelines, match the argument.
     */
    virtual bool validateSourceOperationContext(const OperationContext* opCtx) const {
        return _expCtx->getOperationContext() == opCtx;
    }

protected:
    DocumentSource(StringData stageName, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

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
    virtual DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                           DocumentSourceContainer* container) {
        return std::next(itr);
    };

    /**
     * Utility which describes when a stage needs to nominate a merging shard.
     */
    virtual boost::optional<ShardId> computeMergeShardId() const {
        return boost::none;
    }

    /**
     * Tracks this stage's merge ShardId, if one exists.
     */
    DeferredFn<boost::optional<ShardId>> mergeShardId{[this]() -> boost::optional<ShardId> {
        auto shardId = this->computeMergeShardId();
        tassert(9514400,
                str::stream() << "ShardId must be either boost::none or valid, but got "
                              << (shardId ? shardId->toString() : "boost::none"),
                !shardId || shardId->isValid());
        return shardId;
    }};

    /**
     * unregisterParser_forTest is only meant to be used in the context of unit tests. This is
     * because the parserMap is not thread safe, so modifying it at runtime is unsafe.
     */

    static void unregisterParser_forTest(const std::string& name);

private:
    // Give access to 'getParserMap()' for the implementation of $listMqlEntities but hiding
    // it from all other stages.
    friend class exec::agg::ListMqlEntitiesStage;

    // Used to keep track of which DocumentSources are registered under which name. Initialized
    // during process initialization and const thereafter.
    static StringMap<ParserRegistration> parserMap;

    /**
     * Return the map of curently registered parsers.
     */
    static const StringMap<ParserRegistration>& getParserMap() {
        return parserMap;
    }

    /**
     * Create a Value that represents the document source.
     *
     * This is used by the default implementation of serializeToArray() to add this object
     * to a pipeline being serialized. Returning a missing() Value results in no entry
     * being added to the array for this stage (DocumentSource).
     */
    virtual Value serialize(const SerializationOptions& opts = SerializationOptions{}) const = 0;

    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

}  // namespace mongo
