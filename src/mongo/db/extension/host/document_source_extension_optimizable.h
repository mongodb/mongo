// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host/aggregation_stage/ast_node.h"
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host/catalog_context.h"
#include "mongo/db/extension/host/extension_host_utils.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <string_view>

namespace mongo::extension::host_connector {
class PipelineDependenciesAdapter;
}  // namespace mongo::extension::host_connector

namespace mongo::extension::host {

class DocumentSourceExtensionOptimizableTest;
class LoadExtensionsTest;
class LoadNativeVectorSearchTest;

using LiteParsedList = std::list<std::unique_ptr<LiteParsedDocumentSource>>;

/**
 * StageParams that carries an extension parse node (AggStageParseNodeHandle) before expansion.
 * Used for extension stages that participate in query shape.
 */
class ExpandableStageParams : public StageParams {
public:
    ExpandableStageParams(AggStageParseNodeHandle parseNode, BSONObj originalStage)
        : _parseNode(std::move(parseNode)), _originalStage(originalStage.getOwned()) {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

    AggStageParseNodeHandle releaseParseNode() {
        return std::move(_parseNode);
    }

    BSONObj releaseOriginalStage() {
        return std::move(_originalStage);
    }

private:
    AggStageParseNodeHandle _parseNode;
    // The original user-provided stage object, e.g. {$myStage: {...}}.
    BSONObj _originalStage;
};

/**
 * StageParams that carries an extension AST node (AggStageAstNodeHandle) to be promoted to a
 * logical stage.
 */
class ExpandedStageParams : public StageParams {
public:
    ExpandedStageParams(AggStageAstNodeHandle astNode) : _astNode(std::move(astNode)) {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

    AggStageAstNodeHandle releaseAstNode() {
        return std::move(_astNode);
    }

private:
    AggStageAstNodeHandle _astNode;
};

/**
 * A DocumentSource implementation for an extension aggregation stage.
 * DocumentSourceExtensionOptimizable is the concrete implementation for extension stages that can
 * participate in query optimization and execution.
 */
class DocumentSourceExtensionOptimizable : public DocumentSource {
public:
    /**
     * A LiteParsedDocumentSource implementation for extension stages mapping to an
     * AggStageParseNode.
     */
    class LiteParsedExpandable : public LiteParsedDocumentSource {
    public:
        inline static constexpr int kMaxExpansionDepth = 10;

        static std::unique_ptr<LiteParsedDocumentSource> parse(AggStageDescriptorHandle descriptor,
                                                               const NamespaceString& nss,
                                                               const BSONElement& spec,
                                                               const LiteParserOptions& options) {
            auto parseNode = descriptor->parse(spec.wrap());
            return std::make_unique<LiteParsedExpandable>(spec, std::move(parseNode), nss, options);
        }

        LiteParsedExpandable(const BSONElement& spec,
                             AggStageParseNodeHandle parseNode,
                             const NamespaceString& nss,
                             const LiteParserOptions& options)
            : LiteParsedDocumentSource(spec),
              _parseNode(std::move(parseNode)),
              _nss(nss),
              _options(options),
              _expanded([&] {
                  auto expandedList = expand();
                  tassert(ErrorCodes::ExtensionError,
                          "LiteParsedExpandable must not have an empty expanded pipeline",
                          !expandedList.empty());

                  return StageSpecs(std::make_move_iterator(expandedList.begin()),
                                    std::make_move_iterator(expandedList.end()));
              }()) {}

        std::unique_ptr<StageParams> getStageParams() const override {
            return std::make_unique<ExpandableStageParams>(_parseNode->clone(),
                                                           getOriginalBson().wrap().getOwned());
        }

        /**
         * Return a copy to the pre-computed expanded pipeline.
         */
        StageSpecs getExpandedPipeline() const {
            StageSpecs cloned;
            cloned.reserve(_expanded.size());
            for (const auto& stage : _expanded) {
                cloned.push_back(stage->clone());
            }
            return cloned;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            PrivilegeVector privileges;
            for (const auto& lp : _expanded) {
                Privilege::addPrivilegesToPrivilegeVector(
                    &privileges, lp->requiredPrivileges(isMongos, bypassDocumentValidation));
            }
            return privileges;
        }

        bool isInitialSource() const override {
            return _expanded.front()->isInitialSource();
        }

        bool requiresAuthzChecks() const override {
            for (const auto& lp : _expanded) {
                if (lp->requiresAuthzChecks()) {
                    return true;
                }
            }
            return false;
        }

        bool hasCustomBsonForLog() const override {
            return true;
        }

        BSONObj toBsonForLog() const override {
            return _parseNode->toBsonForLog();
        }

        // TODO SERVER-121094 Remove this override when extensions can handle views through
        // bindResolvedNamespace().
        bool hasExtensionVectorSearchStage() const override;

        // TODO SERVER-121094 Remove this override when extensions can handle views through
        // bindResolvedNamespace().
        bool hasExtensionSearchStage() const override;

        // These checks must consider every expanded stage, as if inlined into the pipeline:
        // ranked/scored if ANY expanded stage is (matching isRankedPipeline()/isScoredPipeline()),
        // selection only if ALL are. isInitialSource() above intentionally stays front-only.
        // TODO SERVER-129047: replace this host-side reduction with the extension-declared
        // ParseNode properties once get_properties is exposed on the ParseNode vtable, and delete
        // these overrides.
        bool isRankedStage() const override {
            return std::any_of(_expanded.begin(), _expanded.end(), [](const auto& stage) {
                return stage->isRankedStage();
            });
        }

        bool isScoredStage() const override {
            return std::any_of(_expanded.begin(), _expanded.end(), [](const auto& stage) {
                return stage->isScoredStage();
            });
        }

        bool isScoreDetailsStage() const override {
            return std::any_of(_expanded.begin(), _expanded.end(), [](const auto& stage) {
                return stage->isScoreDetailsStage();
            });
        }

        bool isSelectionStage() const override {
            return std::all_of(_expanded.begin(), _expanded.end(), [](const auto& stage) {
                return stage->isSelectionStage();
            });
        }

        // Extension stages are unsupported on timeseries collections.
        Constraints constraints() const override {
            return {.canRunOnTimeseries = false};
        }

        // View resolution should happen *after* desugaring, so this should be unreachable.
        FirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const override {
            MONGO_UNREACHABLE_TASSERT(12197200);
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return this->onlyReadConcernLocalSupported(
                this->getParseTimeName(), level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            this->transactionNotSupported(this->getParseTimeName());
        }

        bool isAllowedInLookupPipeline() const override {
            return std::all_of(_expanded.begin(), _expanded.end(), [](const auto& s) {
                return s->isAllowedInLookupPipeline();
            });
        }

        // Define how to desugar a LiteParsedExpandable.
        static LiteParsedDesugarer::StageExpander stageExpander;

    private:
        std::unique_ptr<LiteParsedDocumentSource> _doClone() const override {
            return std::make_unique<LiteParsedExpandable>(
                getOriginalBson(), _parseNode->clone(), _nss, _options);
        }

        /**
         * Carries per-invocation validation state for recursive expansion performed by
         * LiteParsedExpandable. The state is instantiated at the top-level expand() and passed by
         * reference to recursive expandImpl() calls.
         */
        struct ExpansionState {
            // Tracker for the current expansion depth. This is only incremented when expansion
            // results in an ExtensionParseNode that requires further expansion.
            int currDepth = 0;

            // Ordered container of stageNames along the current expansion path.
            std::vector<std::string> expansionPath;

            // Tracker for seen stageNames. Provides O(1) cycle checking.
            stdx::unordered_set<std::string> seenStages;
        };

        /**
         * RAII frame that enforces recursive expansion depth constraints and cycle checking,
         * unwinding on exit.
         */
        class ExpansionValidationFrame;

        LiteParsedList expand();
        static LiteParsedList expandImpl(const AggStageParseNodeHandle& parseNodeHandle,
                                         ExpansionState& state,
                                         const NamespaceString& nss,
                                         const LiteParserOptions& options);

        const AggStageParseNodeHandle _parseNode;
        const NamespaceString _nss;
        const LiteParserOptions _options;
        const StageSpecs _expanded;
    };

    /**
     * A LiteParsedDocumentSource implementation for extension stages mapping to an
     * AggStageAstNode.
     *
     * NOTE: This class is instantiated when LiteParsedDesugarer expands LiteParsedExpandable. From
     * there, LiteParsedExpanded is converted to DocumentSourceExtensionOptimizable for
     * optimization.
     */
    class LiteParsedExpanded : public LiteParsedDocumentSource {
    public:
        LiteParsedExpanded(std::string stageName,
                           AggStageAstNodeHandle astNode,
                           const NamespaceString& nss,
                           std::shared_ptr<IncrementalFeatureRolloutContext> ifrContext = nullptr)
            // NOTE: There is no original BSON since this stage is created from an AST node
            // desugared without BSON.
            : LiteParsedDocumentSource(BSON(stageName << BSONObj())),
              _astNode(std::move(astNode)),
              _properties(_astNode->getProperties()),
              _nss(nss),
              _ifrContext(std::move(ifrContext)) {}

        std::unique_ptr<StageParams> getStageParams() const override {
            return std::make_unique<ExpandedStageParams>(_astNode->clone());
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            PrivilegeVector privileges;

            if (const auto& requiredPrivileges = _properties.getRequiredPrivileges()) {
                for (const auto& rp : *requiredPrivileges) {
                    tassert(
                        ErrorCodes::ExtensionError,
                        "Only 'namespace' resourcePattern is supported for extension privileges",
                        rp.getResourcePattern() ==
                            MongoExtensionPrivilegeResourcePatternEnum::kNamespace);

                    ActionSet actions;
                    for (const auto& entry : rp.getActions()) {
                        actions.addAction(static_properties_util::toActionType(entry.getAction()));
                    }

                    tassert(ErrorCodes::ExtensionError,
                            "requiredPrivileges.actions must not be empty.",
                            !actions.empty());
                    Privilege::addPrivilegeToPrivilegeVector(
                        &privileges, Privilege{ResourcePattern::forExactNamespace(_nss), actions});
                }
            }

            return privileges;
        }

        bool isInitialSource() const override {
            return !_properties.getRequiresInputDocSource();
        }

        bool requiresAuthzChecks() const override {
            // If the stage specifies a non-empty set of required privileges, mandatory auth checks
            // are required. Otherwise, it is safe to opt out of auth checks.
            const auto& properties = _properties.getRequiredPrivileges();
            return properties.has_value() && !properties->empty();
        }

        bool hasExtensionVectorSearchStage() const override;

        bool hasExtensionSearchStage() const override;

        FirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const override;

        void bindResolvedNamespace(const ResolvedNamespace& view,
                                   const ResolvedNamespaceMap& resolvedNamespaces) override;

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return this->onlyReadConcernLocalSupported(
                this->getParseTimeName(), level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            this->transactionNotSupported(this->getParseTimeName());
        }

        bool isAllowedInLookupPipeline() const override {
            return _properties.getAllowedInLookup();
        }

        bool isRankedStage() const override;

        bool isScoredStage() const override;

        bool isScoreDetailsStage() const override;

        bool isSelectionStage() const override;

    private:
        std::unique_ptr<LiteParsedDocumentSource> _doClone() const override {
            return std::make_unique<LiteParsedExpanded>(
                getParseTimeName(), _astNode->clone(), _nss, _ifrContext);
        }

        AggStageAstNodeHandle _astNode;
        const MongoExtensionStaticProperties _properties;
        const NamespaceString _nss;
        std::shared_ptr<IncrementalFeatureRolloutContext> _ifrContext;
    };

    // Construction of a source or transform stage that expanded from a desugar stage. This stage
    // does not hold a parse node and therefore has no concept of a query shape. Its shape
    // responsibility comes from the desugar stage it expanded from.
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, AggStageAstNodeHandle astNode) {
        auto ifrCtx = expCtx->getIfrContext();
        auto hybridSearchFlagEnabled = ifrCtx &&
            ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
        if (expCtx->getInUnionWith() && !hybridSearchFlagEnabled) {
            const auto stageName = std::string(astNode->getName());
            // Throw the IFR retry error for extension $vectorSearch in $unionWith if the feature
            // flag is not enabled.
            search_helpers::throwIfrKickbackIfNecessary(
                search_helpers::isExtensionVectorSearchStage(stageName),
                feature_flags::gFeatureFlagVectorSearchExtension,
                vector_search_metrics::inUnionWithKickbackRetryCount,
                "The $vectorSearch extension stage is not supported in a $unionWith");
            // Throw the IFR retry error for extension $search/$searchMeta in $unionWith if the
            // feature flag is not enabled.
            search_helpers::throwIfrKickbackIfNecessary(
                search_helpers::isExtensionSearchStage(stageName),
                feature_flags::gFeatureFlagSearchExtension,
                search_metrics::inUnionWithKickbackRetryCount,
                "The $search/$searchMeta extension stage is not supported in a $unionWith");
        }

        if (expCtx->getInLookup() && !hybridSearchFlagEnabled) {
            const auto stageName = std::string(astNode->getName());
            // Throw the IFR retry error for extension $vectorSearch in $lookup when
            // featureFlagExtensionsInsideHybridSearch is disabled.
            search_helpers::throwIfrKickbackIfNecessary(
                search_helpers::isExtensionVectorSearchStage(stageName),
                feature_flags::gFeatureFlagVectorSearchExtension,
                vector_search_metrics::inLookupKickbackRetryCount,
                "The $vectorSearch extension stage is not supported in a $lookup");
            // Throw the IFR retry error for extension $search/$searchMeta in $lookup when
            // featureFlagExtensionsInsideHybridSearch is disabled.
            search_helpers::throwIfrKickbackIfNecessary(
                search_helpers::isExtensionSearchStage(stageName),
                feature_flags::gFeatureFlagSearchExtension,
                search_metrics::inLookupKickbackRetryCount,
                "The $search/$searchMeta extension stage is not supported in a $lookup");
        }

        return boost::intrusive_ptr<DocumentSourceExtensionOptimizable>(
            new DocumentSourceExtensionOptimizable(expCtx, std::move(astNode)));
    }

    /**
     * Construct directly from a parse node handle.
     *
     * NOTE: This should only be used when the parse node handle expands into a *single
     * extension-allocated AST node* (e.g. when parsing on a shard after the router has already
     * expanded and serialized the parse node).
     */
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggStageParseNodeHandle& parseNodeHandle);

    /**
     * Construct a DocumentSourceExtensionOptimizable from a logical stage handle.
     *
     * Note: it is important that the input properties match the logical stage type being passed in.
     * Therefore this should only be used when "cloning" an existing document source - e.g. for
     * creating DocumentSources from DPL logical stage handles.
     */
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        LogicalAggStageHandle logicalStage,
        const MongoExtensionStaticProperties& properties) {
        return boost::intrusive_ptr<DocumentSourceExtensionOptimizable>(
            new DocumentSourceExtensionOptimizable(expCtx, std::move(logicalStage), properties));
    }

    /**
     * Recursively expands a parse node handle into a list of DocumentSources. Used during expansion
     * of extension-allocated parse nodes returned from getDistributedPlanLogic().
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> expandParseNode(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggStageParseNodeHandle& parseNodeHandle);

    // This method is invoked by extensions to register descriptor.
    static void registerStage(AggStageDescriptorHandle descriptor);

    std::string_view getSourceName() const override {
        return _stageName;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    Value serialize(const query_shape::SerializationOptions& opts) const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    static const Id& id;

    Id getId() const override;

    bool providesSortKeyMetadata() const override {
        const auto& provided = _properties.getProvidedMetadataFields();
        if (!provided || provided->empty()) {
            return false;
        }
        return std::find(provided->begin(),
                         provided->end(),
                         DocumentMetadataFields::serializeMetaType(
                             DocumentMetadataFields::MetaType::kSortKey)) != provided->end();
    }

    const MongoExtensionStaticProperties& getStaticProperties() const {
        return _properties;
    }

    boost::optional<MongoExtensionDocsNeededBoundsInfo> getDocsNeededBounds() const {
        return _logicalStage->getDocsNeededBounds();
    }

    void skipMetadataStream() override {
        _logicalStage->skipMetadataStream();
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override;

    // Wrapper around the LogicalAggStageHandle::compile() method. Returns an ExecAggStageHandle.
    ExecAggStageHandle compile() {
        return _logicalStage->compile();
    }

    BSONObj getQuery() const override;

    bool hasQuery() const override;

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override;

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    /**
     * Looks up the extension rules for the current extension stage in the extension rule registry
     * and queues them in the RBR engine according to whether the rule's tags match the tag filter.
     * Note that this function ensures that the queued rules remain valid for the entire
     * optimizations pass by storing them in a dedicated structure on
     * DocumentSourceExtensionOptimizable.
     */
    bool dispatchExtensionRules(rule_based_rewrites::pipeline::PipelineRewriteContext& ctx,
                                PipelineRewriteRuleTags tagFilter) const;

    /**
     * Static function that registers each given vector of extension pipeline rewrite rules with the
     * given extension stage name in a static extension rule registry that is populated once at
     * startup and accessible by all DocumentSourceExtensionOptimizable instances.
     */
    static void registerStageRules(std::string_view stageName,
                                   const std::vector<PipelineRewriteRule>& rules);

    static void unregisterStageRules_forTest(std::string_view stageName);
    static const std::vector<PipelineRewriteRule>* getStageRules_forTest(
        std::string_view stageName);

    /**
     * Pushes the pipeline dependencies to the underlying extension logical stage.
     */
    void applyPipelineSuffixDependencies(const host_connector::PipelineDependenciesAdapter& deps);

    /**
     * Builds the boundary adapter and forwards to applyPipelineSuffixDependencies.
     */
    void propagatePipelineSuffixDependencies(const DepsTracker& deps,
                                             const std::set<std::string>& builtinVarRefs) override;

protected:
    /**
     * NB : Here we keep a copy of the stage name to service getSourceName().
     * It is tempting to rely on the name which is provided by the _staticDescriptor, however, that
     * is risky because we need to return a const char* in getSourceName() but a
     * MongoExtensionByteView coming from the extension is not guaranteed to contain a null
     * terminator.
     **/
    const std::string _stageName;
    const MongoExtensionStaticProperties _properties;
    LogicalAggStageHandle _logicalStage;

    DocumentSourceExtensionOptimizable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       AggStageAstNodeHandle astNode)
        : DocumentSourceExtensionOptimizable(
              expCtx,
              [&]() -> LogicalAggStageHandle {
                  tassert(11647800,
                          "DocumentSourceExtensionOptimizable "
                          "received invalid expression context",
                          expCtx.get() != nullptr);
                  auto catalogContext = CatalogContext(*expCtx);
                  return astNode->promote(catalogContext.getAsBoundaryType());
              }(),
              astNode->getProperties()) {}

    DocumentSourceExtensionOptimizable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       LogicalAggStageHandle logicalStage,
                                       const MongoExtensionStaticProperties& properties)
        : DocumentSource(logicalStage->getName(),
                         expCtx,
                         [&]() -> SortPattern {
                             auto bson = logicalStage->getSortPattern();
                             return bson.isEmpty()
                                 ? SortPattern(std::vector<SortPattern::SortPatternPart>{})
                                 : SortPattern(bson, expCtx);
                         }()),
          _stageName(std::string(logicalStage->getName())),
          _properties(properties),
          _logicalStage(std::move(logicalStage)),
          _ownedRewriteRules(_buildOwnedRewriteRules(
              _stageName, UnownedLogicalAggStageHandle(_logicalStage.get()))) {}

private:
    // Do not support copy or move.
    DocumentSourceExtensionOptimizable(const DocumentSourceExtensionOptimizable&) = delete;
    DocumentSourceExtensionOptimizable(DocumentSourceExtensionOptimizable&&) = delete;
    DocumentSourceExtensionOptimizable& operator=(const DocumentSourceExtensionOptimizable&) =
        delete;
    DocumentSourceExtensionOptimizable& operator=(DocumentSourceExtensionOptimizable&&) = delete;

    // Limit value for the pipeline as a whole. This is not the limit that we send
    // to mongot, rather, it is used when adding the $limit stage to the merging
    // pipeline in a sharded cluster. This allows us to limit the documents that
    // are returned from the shards as much as possible without adding complicated
    // rules for pipeline splitting.
    boost::optional<long long> _limit;

    // Shared registry: stageName -> rules, populated during initialization of the extension. This
    // map is not thread-safe and must only be written to at startup.
    static stdx::unordered_map<std::string, std::vector<PipelineRewriteRule>>
        _extensionRuleRegistry;

    // Builds the wrapped host-side rules for this stage from the extension rule registry.
    // Called once during construction.
    static std::vector<rule_based_rewrites::pipeline::PipelineRewriteRule> _buildOwnedRewriteRules(
        const std::string& stageName, UnownedLogicalAggStageHandle logicalStage);

    // Owns the dynamically-created extension rules so that the ctx holds stable pointers into this
    // vector for the lifetime of the optimization pass. This is necessary because the
    // RewriteEngine does not own rules, it holds pointers to them. This is fine for host-defined
    // rules, because they are declared statically. Extension rules, however, are created
    // dynamically at query execution time. Therefore, we must keep them alive here.
    const std::vector<rule_based_rewrites::pipeline::PipelineRewriteRule> _ownedRewriteRules;
};

namespace helper {

template <typename OnParseHost, typename OnParseExt, typename OnAstHost, typename OnAstExt>
inline void visitExpandedNodes(std::vector<VariantNodeHandle>& expanded,
                               OnParseHost&& onParseHost,
                               OnParseExt&& onParseExt,
                               OnAstHost&& onAstHost,
                               OnAstExt&& onAstExt) {
    for (auto& node : expanded) {
        std::visit(
            [&](auto&& handle) {
                using H = std::decay_t<decltype(handle)>;
                // Case 1: Parse node handle.
                //   a) Host-allocated parse node: convert directly to a host
                //      DocumentSource using the host-provided BSON spec. No recursion
                //      in this branch.
                //   b) Extension-allocated parse node: Recurse on the parse node
                //      handle, splicing the results of its expansion.
                if constexpr (std::is_same_v<H, AggStageParseNodeHandle>) {
                    if (host::HostAggStageParseNodeAdapter::isHostAllocated(*handle.get())) {
                        onParseHost(
                            *static_cast<host::HostAggStageParseNodeAdapter*>(handle.get()));
                    } else {
                        onParseExt(handle);
                    }
                }
                // Case 2: AST node handle.
                //   a) Host-allocated AST node: convert directly to a host DocumentSource using
                //      the host-provided BSON spec.
                //   b) Extension-allocated AST node: Construct a
                //      DocumentSourceExtensionOptimizable and release the AST node handle.
                else if constexpr (std::is_same_v<H, AggStageAstNodeHandle>) {
                    if (host::HostAggStageAstNodeAdapter::isHostAllocated(*handle.get())) {
                        onAstHost(*static_cast<host::HostAggStageAstNodeAdapter*>(handle.get()));
                    } else {
                        onAstExt(std::move(handle));
                    }
                }
            },
            node);
    }
}

}  // namespace helper

}  // namespace mongo::extension::host
