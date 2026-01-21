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

#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace mongo {

using StageSpecs = std::vector<std::unique_ptr<LiteParsedDocumentSource>>;

/**
 * A semi-parsed version of a Pipeline, parsed just enough to determine information like what
 * foreign collections are involved.
 */
class MONGO_MOD_PUBLIC LiteParsedPipeline {
public:
    /**
     * Constructs a LiteParsedPipeline from the raw BSON stages given in 'request'.
     *
     * May throw a AssertionException if there is an invalid stage specification, although full
     * validation happens later, during Pipeline construction.
     */
    LiteParsedPipeline(const AggregateCommandRequest& request,
                       const bool isRunningAgainstView_ForHybridSearch = false,
                       const LiteParserOptions& options = LiteParserOptions{})
        : LiteParsedPipeline(request.getNamespace(),
                             request.getPipeline(),
                             isRunningAgainstView_ForHybridSearch,
                             options) {}
    /**
     * Constructs a LiteParsedPipeline from the raw BSON stages in 'pipelineStages'.
     *
     * IMPORTANT: Each stage will store the BSONElement view into the original BSONObj, so the
     * caller is responsible for ensuring the lifetime of the original BSONObj exceeds that of this
     * LiteParsedPipeline. If the original BSON's lifetime cannot be guaranteed, the BSON can be
     * owned by the LiteParsedPipeline by calling makeOwned() after construction.
     */
    LiteParsedPipeline(const NamespaceString& nss,
                       const std::vector<BSONObj>& pipelineStages,
                       const bool isRunningAgainstView_ForHybridSearch = false,
                       const LiteParserOptions& options = LiteParserOptions{})
        : _isRunningAgainstView_ForHybridSearch(isRunningAgainstView_ForHybridSearch) {
        _stageSpecs.reserve(pipelineStages.size());
        for (auto&& rawStage : pipelineStages) {
            auto stageCopy = rawStage;
            if (options.makeSubpipelineOwned) {
                stageCopy.makeOwned();
            }
            _stageSpecs.push_back(LiteParsedDocumentSource::parse(nss, stageCopy, options));
            if (options.makeSubpipelineOwned) {
                _stageSpecs.back()->setOwnedBson(stageCopy);
            }
        }
    }

    /**
     * Copy constructor. Calls clone on each LiteParsedDocumentSource in the pipeline and copies
     * member variables.
     */
    LiteParsedPipeline(const LiteParsedPipeline& other)
        : _isRunningAgainstView_ForHybridSearch(other._isRunningAgainstView_ForHybridSearch),
          _hasChangeStream(&computeHasChangeStream),
          _involvedNamespaces(&computeInvolvedNamespaces) {

        _stageSpecs.reserve(other._stageSpecs.size());
        for (const auto& stage : other._stageSpecs) {
            _stageSpecs.push_back(stage->clone());
        }
    }

    LiteParsedPipeline(LiteParsedPipeline&&) noexcept = default;

    LiteParsedPipeline& operator=(LiteParsedPipeline other) {
        std::swap(_stageSpecs, other._stageSpecs);
        std::swap(_isRunningAgainstView_ForHybridSearch,
                  other._isRunningAgainstView_ForHybridSearch);
        // _hasChangeStream and _involvedNamespaces are Deferred and will be recomputed on demand,
        // so we just reset them to point to our _stageSpecs.
        _hasChangeStream = Deferred<bool (*)(const StageSpecs&)>(&computeHasChangeStream);
        _involvedNamespaces = Deferred<stdx::unordered_set<NamespaceString> (*)(const StageSpecs&)>(
            &computeInvolvedNamespaces);
        return *this;
    }

    LiteParsedPipeline clone() const {
        return LiteParsedPipeline(*this);
    }

    /**
     * Returns all foreign namespaces referenced by stages within this pipeline, if any.
     */
    const stdx::unordered_set<NamespaceString>& getInvolvedNamespaces() const {
        return _involvedNamespaces.get(_stageSpecs);
    }

    /**
     * Returns a vector of the foreign collections(s) referenced by this stage that potentially
     * will be involved in query execution, if any. For example, consider the pipeline:
     *
     * [{$lookup: {from: "bar", localField: "a", foreignField: "b", as: "output"}},
     *  {$unionWith: {coll: "foo", pipeline: [...]}}].
     *
     * Here, "foo" is not considered a foreign execution namespace because "$unionWith" cannot
     * be pushed down into the execution subsystem underneath the leading cursor stage, while
     * "bar" is considered one because "$lookup" can be pushed down in certain cases.
     */
    std::vector<NamespaceStringOrUUID> getForeignExecutionNamespaces() const {
        stdx::unordered_set<NamespaceString> nssSet;
        for (auto&& spec : _stageSpecs) {
            spec->getForeignExecutionNamespaces(nssSet);
        }
        return {nssSet.begin(), nssSet.end()};
    }

    /**
     * Returns a list of the priviliges required for this pipeline.
     */
    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const {
        PrivilegeVector requiredPrivileges;
        for (auto&& spec : _stageSpecs) {
            Privilege::addPrivilegesToPrivilegeVector(
                &requiredPrivileges, spec->requiredPrivileges(isMongos, bypassDocumentValidation));
        }

        return requiredPrivileges;
    }

    /**
     * Returns true if the pipeline begins with a $collStats stage.
     */
    bool startsWithCollStats() const {
        return !_stageSpecs.empty() && _stageSpecs.front()->isCollStats();
    }

    /**
     * Returns true if the pipeline begins with a $indexStats stage.
     */
    bool startsWithIndexStats() const {
        return !_stageSpecs.empty() && _stageSpecs.front()->isIndexStats();
    }

    /**
     * Returns true if the pipeline begins with a stage that generates its own data and should run
     * once.
     */
    bool generatesOwnDataOnce() const {
        return !_stageSpecs.empty() && _stageSpecs.front()->generatesOwnDataOnce();
    }

    /**
     * Returns true if the pipeline ends with a write stage.
     */
    bool endsWithWriteStage() const {
        return !_stageSpecs.empty() && _stageSpecs.back()->isWriteStage();
    }

    /**
     * Returns true if the pipeline has a $changeStream stage.
     */
    bool hasChangeStream() const {
        return _hasChangeStream.get(_stageSpecs);
    }

    /**
     * Returns true if the pipeline has a search stage.
     */
    bool hasSearchStage() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return spec->isSearchStage();
        });
    }

    /**
     * Returns true if the pipeline has a vector search stage.
     */
    bool hasExtensionVectorSearchStage() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return spec->isExtensionVectorSearchStage();
        });
    }

    /**
     * Returns true iff the pipeline has a $rankFusion or $scoreFusion stage.
     */
    bool hasHybridSearchStage() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return spec->isHybridSearchStage();
        });
    }

    /**
     * Returns true iff the pipeline has a $score stage.
     */
    bool hasScoreStage() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return (spec->getParseTimeName() == "$score");
        });
    }

    /**
     * Returns true if the pipeline contains at least one stage that requires the aggregation
     * command to be exempt from ingress admission control.
     */
    bool isExemptFromIngressAdmissionControl() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return spec->isExemptFromIngressAdmissionControl();
        });
    }

    /**
     * Returns true if any of the stages in this pipeline require knowledge of the collection
     * default collation to be successfully parsed, false otherwise. Note that this only applies
     * to top level stages and does not account for subpipelines.
     * TODO SERVER-81991: Delete this function once all unsharded collections are tracked in the
     * sharding catalog as unsplittable along with their collation.
     */
    bool requiresCollationForParsingUnshardedAggregate() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return spec->requiresCollationForParsingUnshardedAggregate();
        });
    }

    /**
     * Returns an error Status if at least one of the stages does not allow the involved
     * namespace 'nss' to be sharded, otherwise returns Status::OK().
     */
    Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                          bool isMultiDocumentTransaction) const {
        for (auto&& spec : _stageSpecs) {
            if (auto status = spec->checkShardedForeignCollAllowed(nss, isMultiDocumentTransaction);
                !status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    /**
     * Verifies that this pipeline is allowed to run with the specified read concern level.
     */
    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault,
                                                 bool explain) const;

    /**
     * Checks that all of the stages in this pipeline are allowed to run with the specified read
     * concern level. Does not do any pipeline global checks.
     */
    ReadConcernSupportResult sourcesSupportReadConcern(repl::ReadConcernLevel level,
                                                       bool isImplicitDefault) const;

    /**
     * Verifies that this pipeline is allowed to run in a multi-document transaction. This
     * ensures that each stage is compatible, and throws a UserException if not. This should
     * only be called if the caller has determined the current operation is part of a
     * transaction.
     */
    void assertSupportsMultiDocumentTransaction(bool explain) const;

    /**
     * Verifies that this pipeline is allowed to run with the read concern from the provided
     * opCtx. Used only when asserting is the desired behavior, otherwise use
     * supportsReadConcern instead.
     */
    void assertSupportsReadConcern(OperationContext* opCtx, bool explain) const;

    /**
     * Perform checks that verify that the LitePipe is valid. Note that this function must be
     * called before forwarding an aggregation command on an unsharded collection, in order to
     * verify that the involved namespaces are allowed to be sharded.
     */
    void verifyIsSupported(OperationContext* opCtx,
                           std::function<bool(OperationContext*, const NamespaceString&)> isSharded,
                           bool explain) const;

    /**
     * Returns true if the first stage in the pipeline does not require an input source.
     */
    bool startsWithInitialSource() const {
        return !_stageSpecs.empty() && _stageSpecs.front()->isInitialSource();
    }

    /**
     * Increments global stage counters corresponding to the stages in this lite parsed
     * pipeline.
     */
    void tickGlobalStageCounters() const;

    /**
     * Verifies that the pipeline contains valid stages. Optionally calls
     * 'validatePipelineStagesforAPIVersion' with 'opCtx'.
     */
    void validate(const OperationContext* opCtx, bool performApiVersionChecks = true) const;

    /**
     * Checks that specific stage types are not present in the pipeline that are disallowed
     * in the definition of a view. Recursively checks sub-pipelines.
     */
    void checkStagesAllowedInViewDefinition() const;

    // TODO SERVER-101722: Remove this once the validation is changed.
    bool isRunningAgainstView_ForHybridSearch() const {
        return _isRunningAgainstView_ForHybridSearch;
    }

    const StageSpecs& getStages() const {
        return _stageSpecs;
    }

    /**
     * Replaces the stage at 'index' with the stages in 'newSources'. Returns the index after the
     * inserted block.
     */
    size_t replaceStageWith(size_t index,
                            std::vector<std::unique_ptr<LiteParsedDocumentSource>>&& newSources);

    /**
     * Resets the deferred caches for hasChangeStream and involvedNamespaces. Should be called after
     * _stageSpecs has been modified.
     */
    void resetDeferredCaches() {
        _hasChangeStream = Deferred<bool (*)(const StageSpecs&)>(&computeHasChangeStream);
        _involvedNamespaces = Deferred<stdx::unordered_set<NamespaceString> (*)(const StageSpecs&)>(
            &computeInvolvedNamespaces);
    }

    /**
     * Converts all child LiteParsedDocumentSources to own the BSON they hold, similar to
     * BSONObj::getOwned(). This should be called when the source BSONObjs' lifetimes cannot be
     * guaranteed to exceed that of this instance.
     */
    void makeOwned() {
        for (auto& stage : _stageSpecs) {
            stage->makeOwned();
        }
    }

    /**
     * Applies view semantics to this pipeline.
     *
     * Each stage is given a chance to validate the view or modify itself with the view via its
     * ViewPolicy callback. Whether the view pipeline is automatically prepended is determined
     * solely by the first stage: if its policy is kDefaultPrepend (or the pipeline is empty), the
     * desugared view pipeline is cloned and prepended; otherwise it is not.
     *
     * The provided ViewInfo is not mutated.
     */
    void handleView(const ViewInfo& viewInfo);

private:
    friend struct ViewInfo;

    // This is logically const - any changes to _stageSpecs will invalidate cached copies of
    // "_hasChangeStream" and "_involvedNamespaces" below.
    StageSpecs _stageSpecs;

    // This variable specifies whether the pipeline is running on a view's namespace. This is
    // currently needed for $rankFusion/$scoreFusion positional validation.
    // TODO SERVER-101722: Remove this once the validation is changed.
    bool _isRunningAgainstView_ForHybridSearch = false;

    /**
     * Prepend 'prefix' stages in front of this pipeline, taking ownership of prefix.
     * Resets any derived caches.
     */
    void _stitchFront(LiteParsedPipeline&& prefix);

    static bool computeHasChangeStream(const StageSpecs& stageSpecs) {
        return std::any_of(stageSpecs.begin(), stageSpecs.end(), [](const auto& spec) {
            return spec->isChangeStream();
        });
    }

    static stdx::unordered_set<NamespaceString> computeInvolvedNamespaces(
        const StageSpecs& stageSpecs) {
        stdx::unordered_set<NamespaceString> involved;
        for (const auto& spec : stageSpecs) {
            auto ns = spec->getInvolvedNamespaces();
            involved.insert(ns.begin(), ns.end());
        }
        return involved;
    }

    Deferred<bool (*)(const StageSpecs&)> _hasChangeStream{&computeHasChangeStream};

    Deferred<stdx::unordered_set<NamespaceString> (*)(const StageSpecs&)> _involvedNamespaces{
        &computeInvolvedNamespaces};
};

}  // namespace mongo
