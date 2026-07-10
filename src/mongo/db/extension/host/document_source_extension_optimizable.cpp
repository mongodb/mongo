/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/host/document_source_extension_optimizable.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/extension/host/document_source_extension_for_query_shape.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/adapter/pipeline_dependencies_adapter.h"
#include "mongo/db/extension/host_connector/adapter/pipeline_rewrite_context_adapter.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/host_connector/adapter/resolved_namespace_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/extension/shared/handle/pipeline_rewrite_context_handle.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/vector_search_helper.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo::extension::host {
using namespace std::literals::string_view_literals;

ALLOCATE_STAGE_PARAMS_ID(expandable, ExpandableStageParams::id);

ALLOCATE_STAGE_PARAMS_ID(expanded, ExpandedStageParams::id);

DocumentSourceContainer expandableStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* expandableParams = static_cast<ExpandableStageParams*>(stageParams.get());
    auto parseNode = expandableParams->releaseParseNode();
    auto originalStage = expandableParams->releaseOriginalStage();

    return {DocumentSourceExtensionForQueryShape::create(
        expCtx, std::move(parseNode), std::move(originalStage))};
}

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(expandable,
                                                 ExpandableStageParams::id,
                                                 expandableStageParamsToDocumentSourceFn);

DocumentSourceContainer expandedStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* extensionParams = static_cast<ExpandedStageParams*>(stageParams.get());
    return {DocumentSourceExtensionOptimizable::create(expCtx, extensionParams->releaseAstNode())};
}

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(expanded,
                                                 ExpandedStageParams::id,
                                                 expandedStageParamsToDocumentSourceFn);

class DocumentSourceExtensionOptimizable::LiteParsedExpandable::ExpansionValidationFrame {
public:
    ExpansionValidationFrame(ExpansionState& state, std::string stageName)
        : _state(state), _stageName(std::move(stageName)) {
        const auto newDepth = _state.currDepth + 1;
        tassert(ErrorCodes::ExtensionError,
                str::stream() << "Stage expansion exceeded maximum depth of " << kMaxExpansionDepth,
                newDepth <= kMaxExpansionDepth);

        const auto inserted = _state.seenStages.insert(_stageName).second;
        tassert(ErrorCodes::ExtensionError,
                str::stream() << "Cycle detected during stage expansion for stage " << _stageName
                              << ": " << _formatCyclePath(_state, _stageName),
                inserted);

        _state.expansionPath.push_back(_stageName);
        _state.currDepth = newDepth;
    }

    ~ExpansionValidationFrame() noexcept {
        _state.seenStages.erase(_stageName);
        _state.expansionPath.pop_back();
        _state.currDepth--;
    }

    // Non-copyable, non-movable.
    ExpansionValidationFrame(const ExpansionValidationFrame&) = delete;
    ExpansionValidationFrame& operator=(const ExpansionValidationFrame&) = delete;

private:
    /**
     * Format the cyclical slice for an informative error message (e.g. A -> B -> A).
     */
    static std::string _formatCyclePath(const ExpansionState& state, const std::string& stageName) {
        size_t start = 0;
        // Find the position of the first occurrence of the cyclical stage.
        for (; start < state.expansionPath.size(); ++start) {
            if (state.expansionPath[start] == stageName) {
                break;
            }
        }

        StringBuilder sb;
        constexpr std::string_view arrow = " -> "sv;
        // Construct the path starting from the first occurrence.
        for (size_t i = start; i < state.expansionPath.size(); ++i) {
            if (i > start) {
                sb << arrow;
            }
            sb << state.expansionPath[i];
        }
        // Close the loop with the cyclical stage.
        sb << arrow << stageName;
        return sb.str();
    }

    ExpansionState& _state;
    std::string _stageName;
};

LiteParsedList DocumentSourceExtensionOptimizable::LiteParsedExpandable::expand() {
    ExpansionState state;
    return expandImpl(_parseNode, state, _nss, _options);
}

LiteParsedList DocumentSourceExtensionOptimizable::LiteParsedExpandable::expandImpl(
    const AggStageParseNodeHandle& parseNodeHandle,
    ExpansionState& state,
    const NamespaceString& nss,
    const LiteParserOptions& options) {
    LiteParsedList outExpanded;
    auto expanded = parseNodeHandle->expand();

    helper::visitExpandedNodes(
        expanded,
        [&](const HostAggStageParseNodeAdapter& hostParse) {
            const auto& spec = hostParse.getBsonSpec();
            auto lpds = LiteParsedDocumentSource::parse(nss, spec, options);
            lpds->makeOwned();
            outExpanded.emplace_back(std::move(lpds));
        },
        [&](const AggStageParseNodeHandle& handle) {
            const auto stageName = std::string(handle->getName());
            ExpansionValidationFrame frame{state, stageName};
            auto children = expandImpl(handle, state, nss, options);
            outExpanded.splice(outExpanded.end(), children);
        },
        [&](const HostAggStageAstNodeAdapter& hostAst) {
            outExpanded.emplace_back(hostAst.expandToLiteParsed(nss, options));
        },
        [&](AggStageAstNodeHandle handle) {
            outExpanded.emplace_back(std::make_unique<LiteParsedExpanded>(
                std::string(handle->getName()), std::move(handle), nss, options.ifrContext));
        });

    return outExpanded;
}

LiteParsedDesugarer::StageExpander
    DocumentSourceExtensionOptimizable::LiteParsedExpandable::stageExpander =
        [](LiteParsedPipeline* pipeline, size_t index, LiteParsedDocumentSource& stage) {
            auto& expandable =
                static_cast<DocumentSourceExtensionOptimizable::LiteParsedExpandable&>(stage);
            auto expanded = expandable.getExpandedPipeline();

            // Replace the one LPDS with its desugared form; return next index.
            return pipeline->replaceStageWith(index, std::move(expanded));
        };

MONGO_INITIALIZER_WITH_PREREQUISITES(RegisterStageExpanderForLiteParsedExtensionExpandable,
                                     ("EndStageIdAllocation"))
(InitializerContext*) {
    tassert(11533001,
            "ExpandableStageParams::id must be allocated before registering expander",
            ExpandableStageParams::id != StageParams::kUnallocatedId);
    LiteParsedDesugarer::registerStageExpander(
        ExpandableStageParams::id,
        DocumentSourceExtensionOptimizable::LiteParsedExpandable::stageExpander);
}

// TODO SERVER-121094 Remove this check when the extension can do this through
// bindResolvedNamespace().
bool DocumentSourceExtensionOptimizable::LiteParsedExpandable::hasExtensionVectorSearchStage()
    const {
    return search_helpers::isExtensionVectorSearchStage(getParseTimeName());
}

// TODO SERVER-121094 Remove this check when the extension can do this through
// bindResolvedNamespace().
bool DocumentSourceExtensionOptimizable::LiteParsedExpandable::hasExtensionSearchStage() const {
    return search_helpers::isExtensionSearchStage(getParseTimeName());
}

// TODO SERVER-121094 Remove this check when the extension can do this through
// bindResolvedNamespace().
bool DocumentSourceExtensionOptimizable::LiteParsedExpanded::hasExtensionVectorSearchStage() const {
    return search_helpers::isExtensionVectorSearchStage(getParseTimeName());
}

// TODO SERVER-121094 Remove this check when the extension can do this through
// bindResolvedNamespace().
bool DocumentSourceExtensionOptimizable::LiteParsedExpanded::hasExtensionSearchStage() const {
    return search_helpers::isExtensionSearchStage(getParseTimeName());
}

FirstStageViewApplicationPolicy
DocumentSourceExtensionOptimizable::LiteParsedExpanded::getFirstStageViewApplicationPolicy() const {
    return view_util::toFirstStageApplicationPolicy(_astNode->getFirstStageViewApplicationPolicy());
}

void DocumentSourceExtensionOptimizable::LiteParsedExpanded::bindResolvedNamespace(
    const ResolvedNamespace& view, const ResolvedNamespaceMap& resolvedNamespaces) {
    if (view.getNamespace().isEmpty()) {
        // An empty view namespace means this stage is being notified that its pipeline is *not*
        // running on a view. Skip the view-policy checks and the SDK bindResolvedNamespace handoff.
        // TODO SERVER-125741 Enable extensions to bind to non-top-level involved namespaces.
        return;
    }
    auto hybridSearchFlagEnabled = _ifrContext &&
        _ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (!hybridSearchFlagEnabled) {
        // Only $vectorSearch and $search/$searchMeta support IFR kickback on views.
        // All other extension stages are banned on views when the feature flag is disabled.
        uassert(ErrorCodes::NotImplemented,
                str::stream() << "Extension stages are not allowed to run on a view namespace.",
                hasExtensionVectorSearchStage() || hasExtensionSearchStage());

        // Throw an IFR retry error for extension $vectorSearch on a view.
        search_helpers::throwIfrKickbackIfNecessary(
            hasExtensionVectorSearchStage(),
            feature_flags::gFeatureFlagVectorSearchExtension,
            vector_search_metrics::onViewKickbackRetryCount,
            "$vectorSearch-as-an-extension is not allowed against views.");
        // Throw an IFR retry error for extension $search/$searchMeta on a view.
        search_helpers::throwIfrKickbackIfNecessary(
            hasExtensionSearchStage(),
            feature_flags::gFeatureFlagSearchExtension,
            search_metrics::onViewKickbackRetryCount,
            "$search/$searchMeta-as-an-extension are not allowed against views.");
    }

    auto resolvedNamespaceAdapter =
        host_connector::ResolvedNamespaceAdapter::fromResolvedNamespace(view);
    _astNode->bindResolvedNamespace(resolvedNamespaceAdapter.getAsBoundaryType());
}

bool DocumentSourceExtensionOptimizable::LiteParsedExpanded::isRankedStage() const {
    const auto& provided = _properties.getProvidedMetadataFields();
    if (!provided || provided->empty()) {
        return false;
    }
    return std::find(provided->begin(),
                     provided->end(),
                     DocumentMetadataFields::serializeMetaType(
                         DocumentMetadataFields::MetaType::kSortKey)) != provided->end();
}

bool DocumentSourceExtensionOptimizable::LiteParsedExpanded::isScoredStage() const {
    const auto& provided = _properties.getProvidedMetadataFields();
    if (!provided || provided->empty()) {
        return false;
    }
    return std::any_of(
        provided->begin(), provided->end(), DocumentMetadataFields::isScoreProducingMetaType);
}

bool DocumentSourceExtensionOptimizable::LiteParsedExpanded::isScoreDetailsStage() const {
    const auto& provided = _properties.getProvidedMetadataFields();
    if (!provided || provided->empty()) {
        return false;
    }
    return std::any_of(provided->begin(),
                       provided->end(),
                       DocumentMetadataFields::isScoreDetailsProducingMetaType);
}

bool DocumentSourceExtensionOptimizable::LiteParsedExpanded::isSelectionStage() const {
    return _properties.getIsSelectionStage();
}

// static
void DocumentSourceExtensionOptimizable::registerStage(AggStageDescriptorHandle descriptor) {
    auto nameStringData = descriptor->getName();
    auto stageName = std::string(nameStringData);

    using LiteParseFn = std::function<std::unique_ptr<LiteParsedDocumentSource>(
        const NamespaceString&, const BSONElement&, const LiteParserOptions&)>;

    auto parser = [&]() -> LiteParseFn {
        return [descriptor](const NamespaceString& nss,
                            const BSONElement& spec,
                            const LiteParserOptions& opts) {
            return DocumentSourceExtensionOptimizable::LiteParsedExpandable::parse(
                descriptor, nss, spec, opts);
        };
    }();

    LiteParsedDocumentSource::registerParser(
        stageName,
        {.parser = std::move(parser),
         .fromExtension = true,
         .allowedWithApiStrict = AllowedWithApiStrict::kAlways,
         .allowedWithClientType =
             descriptor_util::toAllowedWithClientType(descriptor->getClientType())});
}

ALLOCATE_DOCUMENT_SOURCE_ID(extensionOptimizable, DocumentSourceExtensionOptimizable::id);

Value DocumentSourceExtensionOptimizable::serialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        auto wrappedCtx = std::make_unique<QueryExecutionContext>(getExpCtx().get());
        host_connector::QueryExecutionContextAdapter ctxAdapter(std::move(wrappedCtx));
        return Value(_logicalStage->explain(ctxAdapter, *opts.verbosity));
    }

    // Serialize the stage for query execution.
    return Value(_logicalStage->serialize());
}

StageConstraints DocumentSourceExtensionOptimizable::constraints(
    PipelineSplitState pipeState) const {
    auto constraints =
        StageConstraints(static_properties_util::toStreamType(_properties.getStreamType()),
                         PositionRequirement::kNone,
                         HostTypeRequirement::kNone,
                         DiskUseRequirement::kNoDiskUse,
                         FacetRequirement::kNotAllowed,
                         TransactionRequirement::kNotAllowed,
                         LookupRequirement::kAllowed,
                         UnionRequirement::kAllowed,
                         ChangeStreamRequirement::kDenylist);
    constraints.canRunOnTimeseries = false;

    // Apply potential overrides from static properties.
    if (!_properties.getRequiresInputDocSource()) {
        constraints.setConstraintsForNoInputSources();
    }
    if (auto pos = static_properties_util::toPositionRequirement(_properties.getPosition())) {
        constraints.requiredPosition = *pos;
    }
    if (auto host = static_properties_util::toHostTypeRequirement(_properties.getHostType())) {
        constraints.hostRequirement = *host;
    }
    if (!_properties.getAllowedInUnionWith()) {
        constraints.unionRequirement = StageConstraints::UnionRequirement::kNotAllowed;
    }
    auto ifrCtx = getExpCtx()->getIfrContext();
    const bool hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (!_properties.getAllowedInLookup() || !hybridSearchFlagEnabled) {
        constraints.lookupRequirement = StageConstraints::LookupRequirement::kNotAllowed;
    }

    // TODO SERVER-117260 Enable extension stages in $facet; change the default back to 'kAllowed'.
    // if (!_properties.getAllowedInFacet()) {
    //     constraints.facetRequirement = StageConstraints::FacetRequirement::kNotAllowed;
    // }

    return constraints;
}

DocumentSource::Id DocumentSourceExtensionOptimizable::getId() const {
    return id;
}

DepsTracker::State DocumentSourceExtensionOptimizable::getDependencies(DepsTracker* deps) const {
    auto processFields = [&](const auto& fields, auto propertyName, auto&& apply) {
        if (fields.has_value()) {
            for (const auto& fieldName : *fields) {
                uassert(12489100,
                        str::stream() << "Extension stage '" << getSourceName() << "' declared '"
                                      << fieldName << "' in " << propertyName
                                      << ", which is not a recognized metadata field",
                        DocumentMetadataFields::isValidMetaType(fieldName));
                apply(DocumentMetadataFields::parseMetaType(fieldName));
            }
        }
    };

    // Report required metadata fields for this stage.
    processFields(_properties.getRequiredMetadataFields(),
                  "requiredMetadataFields",
                  [&](auto metaType) { deps->setNeedsMetadata(metaType); });

    // Drop upstream metadata fields if this stage does not preserve them.
    if (!_properties.getPreservesUpstreamMetadata()) {
        // TODO: SERVER-100443
        deps->clearMetadataAvailable();
    }

    // Report provided metadata fields for this stage.
    processFields(_properties.getProvidedMetadataFields(),
                  "providedMetadataFields",
                  [&](auto metaType) { deps->setMetadataAvailable(metaType); });

    // Return SEE_NEXT to ensure metadata dependencies are propagated to the pipeline.
    // Returning NOT_SUPPORTED would prevent our metadata requests from being honored.
    // We still need whole document since extensions may access any fields.
    deps->needWholeDocument = true;
    return DepsTracker::State::SEE_NEXT;
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceExtensionOptimizable::distributedPlanLogic(const DistributedPlanContext* ctx) {
    auto dplHandle = _logicalStage->getDistributedPlanLogic();

    if (!dplHandle.isValid()) {
        return boost::none;
    }

    // Convert the returned VariantDPLHandle to a list of DocumentSources.
    const auto convertDPLHandleToDocumentSources = [&](VariantDPLHandle& handle) {
        return std::visit(
            OverloadedVisitor{
                [&](AggStageParseNodeHandle& dplElement) {
                    if (HostAggStageParseNodeAdapter::isHostAllocated(*dplElement.get())) {
                        // Host-allocated: parse the host-allocated parse node.
                        const auto& hostParse =
                            *static_cast<const HostAggStageParseNodeAdapter*>(dplElement.get());
                        const auto& bsonSpec = hostParse.getBsonSpec();

                        // Validate that the host parse node does not contain an extension stage.
                        auto stageName = bsonSpec.firstElementFieldNameStringData();
                        tassert(ErrorCodes::ExtensionError,
                                str::stream() << "Extension stage '" << getSourceName()
                                              << "' returned an invalid distributedPlanLogic: the "
                                                 "host parse node contains extension stage '"
                                              << stageName,
                                !LiteParsedDocumentSource::isRegisteredExtensionStage(stageName));

                        return DocumentSource::parse(getExpCtx(), bsonSpec);
                    } else {
                        // Extension-allocated: expand the parse node.
                        return expandParseNode(getExpCtx(), dplElement);
                    }
                },
                [&](LogicalAggStageHandle& dplLogicalStage) {
                    // Create a DocumentSource directly from the logical stage handle. We only allow
                    // logical stages to be created here if they are the same type as the
                    // originating stage. Because of this assumption, we can pass in the static
                    // properties from the originating stage. Otherwise we would not have access to
                    // the new stage's properties here, since they live on the ASTNode.
                    tassert(ErrorCodes::ExtensionError,
                            "an extension logical stage in a distributed plan pipeline must be the "
                            "same type as its originating stage",
                            dplLogicalStage->getName() == _logicalStage->getName());
                    return std::list<boost::intrusive_ptr<DocumentSource>>{
                        DocumentSourceExtensionOptimizable::create(
                            getExpCtx(), std::move(dplLogicalStage), _properties)};
                }},
            handle);
    };

    DistributedPlanLogic logic;

    // Convert shardsPipeline.
    auto shardsPipeline = dplHandle->extractShardsPipeline();
    if (!shardsPipeline.empty()) {
        tassert(ErrorCodes::ExtensionError,
                "Shards pipeline must have exactly one element per API specification",
                shardsPipeline.size() == 1);
        auto shardsStages = convertDPLHandleToDocumentSources(shardsPipeline[0]);
        tassert(ErrorCodes::ExtensionError,
                "Single shardsStage must expand to exactly one DocumentSource",
                shardsStages.size() == 1);
        logic.shardsStage = shardsStages.front();
    }

    // Convert mergingPipeline.
    auto mergingPipeline = dplHandle->extractMergingPipeline();
    for (auto& handle : mergingPipeline) {
        auto stages = convertDPLHandleToDocumentSources(handle);
        logic.mergingStages.splice(logic.mergingStages.end(), stages);
    }

    // Convert sortPattern.
    const auto sortPattern = dplHandle->getSortPattern();
    if (!sortPattern.isEmpty()) {
        logic.mergeSortPattern = sortPattern.getOwned();
        uassert(12489101,
                str::stream() << "Extension stage '" << getSourceName()
                              << "' returned a distributedPlanLogic with a merge sort pattern but "
                                 "does not declare sortKey in its providedMetadataFields; a stage "
                                 "that produces a merge sort pattern must also provide sortKey "
                                 "metadata on every document",
                providesSortKeyMetadata());
    }

    return logic;
}

boost::intrusive_ptr<DocumentSourceExtensionOptimizable> DocumentSourceExtensionOptimizable::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggStageParseNodeHandle& parseNodeHandle) {
    auto expanded = parseNodeHandle->expand();

    tassert(ErrorCodes::ExtensionError,
            "Expected parseNode to only expand into a single node.",
            expanded.size() == 1);

    boost::intrusive_ptr<DocumentSourceExtensionOptimizable> optimizable = nullptr;
    helper::visitExpandedNodes(
        expanded,
        [&](const HostAggStageParseNodeAdapter& host) {
            tasserted(ErrorCodes::ExtensionError,
                      "Expected extension AST node, got host parse node.");
        },
        [&](const AggStageParseNodeHandle& handle) {
            tasserted(ErrorCodes::ExtensionError,
                      "Expected extension AST node, got extension parse node.");
        },
        [&](const HostAggStageAstNodeAdapter& hostAst) {
            tasserted(ErrorCodes::ExtensionError,
                      "Expected extension AST node, got host AST node.");
        },
        [&](AggStageAstNodeHandle handle) {
            optimizable = DocumentSourceExtensionOptimizable::create(expCtx, std::move(handle));
        });

    return optimizable;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceExtensionOptimizable::expandParseNode(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggStageParseNodeHandle& parseNodeHandle) {
    std::list<boost::intrusive_ptr<DocumentSource>> outExpanded;
    std::vector<VariantNodeHandle> expanded = parseNodeHandle->expand();

    helper::visitExpandedNodes(
        expanded,
        [&](const HostAggStageParseNodeAdapter& host) {
            const BSONObj& bsonSpec = host.getBsonSpec();
            outExpanded.splice(outExpanded.end(), DocumentSource::parse(expCtx, bsonSpec));
        },
        [&](const AggStageParseNodeHandle& handle) {
            std::list<boost::intrusive_ptr<DocumentSource>> children =
                expandParseNode(expCtx, handle);
            outExpanded.splice(outExpanded.end(), children);
        },
        [&](const HostAggStageAstNodeAdapter& hostAst) {
            outExpanded.splice(outExpanded.end(), hostAst.expandToDocumentSource(expCtx));
        },
        [&](AggStageAstNodeHandle handle) {
            outExpanded.emplace_back(
                DocumentSourceExtensionOptimizable::create(expCtx, std::move(handle)));
        });

    return outExpanded;
}

BSONObj DocumentSourceExtensionOptimizable::getQuery() const {
    if (!feature_flags::gFeatureFlagExtensionsOptimizations.isEnabled()) {
        return BSONObj();
    }

    return _logicalStage->getFilter();
}

bool DocumentSourceExtensionOptimizable::hasQuery() const {
    return !getQuery().isEmpty();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceExtensionOptimizable::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    return create(newExpCtx, _logicalStage->clone(), _properties);
}

DocumentSourceContainer::iterator DocumentSourceExtensionOptimizable::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // The REDUNDANT_SORT_REMOVAL rule takes care of $extensionVectorSearch's desired $sort
    // optimization.
    if (!feature_flags::gFeatureFlagExtensionsOptimizations.isEnabled()) {
        _limit = search_helpers::setVectorSearchLimitForOptimization(itr, container, _limit);
        _logicalStage->setExtractedLimitVal_deprecated(_limit);
    }
    return std::next(itr);
}

stdx::unordered_map<std::string, std::vector<PipelineRewriteRule>>
    DocumentSourceExtensionOptimizable::_extensionRuleRegistry;

// static
void DocumentSourceExtensionOptimizable::registerStageRules(
    std::string_view stageName, const std::vector<extension::PipelineRewriteRule>& rules) {
    auto [_, inserted] = _extensionRuleRegistry.emplace(stageName, rules);
    tassert(12201405, "Rules already registered for stage: " + std::string{stageName}, inserted);
}

// static
void DocumentSourceExtensionOptimizable::unregisterStageRules_forTest(std::string_view stageName) {
    _extensionRuleRegistry.erase(std::string_view(stageName));
}

// static
const std::vector<PipelineRewriteRule>* DocumentSourceExtensionOptimizable::getStageRules_forTest(
    std::string_view stageName) {
    auto it = _extensionRuleRegistry.find(std::string_view(stageName));
    if (it == _extensionRuleRegistry.end()) {
        return nullptr;
    }
    return &it->second;
}

// static
std::vector<rule_based_rewrites::pipeline::PipelineRewriteRule>
DocumentSourceExtensionOptimizable::_buildOwnedRewriteRules(
    const std::string& stageName, UnownedLogicalAggStageHandle logicalStage) {
    auto it = _extensionRuleRegistry.find(stageName);
    if (it == _extensionRuleRegistry.end()) {
        return {};
    }
    std::vector<rule_based_rewrites::pipeline::PipelineRewriteRule> rules;
    rules.reserve(it->second.size());
    for (const auto& rule : it->second) {
        rules.push_back(host_connector::wrapExtensionRule(rule, logicalStage));
    }
    return rules;
}

bool DocumentSourceExtensionOptimizable::dispatchExtensionRules(
    rule_based_rewrites::pipeline::PipelineRewriteContext& ctx,
    PipelineRewriteRuleTags tagFilter) const {
    for (const auto& rule : _ownedRewriteRules) {
        if (rule.tags & tagFilter) {
            ctx.addRule(rule);
        }
    }
    return false;
}

void DocumentSourceExtensionOptimizable::applyPipelineSuffixDependencies(
    const host_connector::PipelineDependenciesAdapter& deps) {
    _logicalStage->applyPipelineSuffixDependencies(&deps);
}

void DocumentSourceExtensionOptimizable::propagatePipelineSuffixDependencies(
    const DepsTracker& deps, const std::set<std::string>& builtinVarRefs) {
    const host_connector::PipelineDependenciesAdapter adapter(deps, builtinVarRefs);
    applyPipelineSuffixDependencies(adapter);
}

bool extensionDispatcherReorderingPrecondition(
    rule_based_rewrites::pipeline::PipelineRewriteContext& ctx) {
    const auto* stage = dynamic_cast<const DocumentSourceExtensionOptimizable*>(&ctx.current());
    return stage && stage->dispatchExtensionRules(ctx, kReordering);
}

bool extensionDispatcherInPlacePrecondition(
    rule_based_rewrites::pipeline::PipelineRewriteContext& ctx) {
    const auto* stage = dynamic_cast<const DocumentSourceExtensionOptimizable*>(&ctx.current());
    return stage && stage->dispatchExtensionRules(ctx, kInPlace);
}

bool extensionApplyDependenciesPrecondition(
    rule_based_rewrites::pipeline::PipelineRewriteContext& ctx) {
    const auto* stage = dynamic_cast<const DocumentSourceExtensionOptimizable*>(&ctx.current());
    // Only source stages produce documents that benefit from dependency analysis. Note that this is
    // only run when the full pipeline is visible (i.e. not on a shard after pipeline split).
    return stage && !stage->getStaticProperties().getRequiresInputDocSource() &&
        !ctx.getExpCtx().getNeedsMerge();
}

bool extensionApplyDependenciesTransform(
    rule_based_rewrites::pipeline::PipelineRewriteContext& ctx) {
    auto* stage = dynamic_cast<DocumentSourceExtensionOptimizable*>(&ctx.current());
    const host_connector::PipelineDependenciesAdapter adapter(
        ctx.getPipelineSuffixDependencies(), ctx.getBuiltInVariableRefsInPipelineSuffix());
    stage->applyPipelineSuffixDependencies(adapter);
    return false;
}

}  // namespace mongo::extension::host

namespace mongo::rule_based_rewrites::pipeline {
using DocumentSourceExtensionOptimizable =
    mongo::extension::host::DocumentSourceExtensionOptimizable;
REGISTER_RULES_WITH_FEATURE_FLAG(
    DocumentSourceExtensionOptimizable,
    &feature_flags::gFeatureFlagExtensionsOptimizations,
    {
        .name = "EXTENSION_DISPATCHER_REORDERING",
        .precondition = mongo::extension::host::extensionDispatcherReorderingPrecondition,
        .transform = Transforms::noop,
        .priority = kDefaultOptimizeAtPriority + 1,
        .tags = PipelineRewriteContext::Tags::Reordering,
    },
    {
        .name = "EXTENSION_DISPATCHER_IN_PLACE",
        .precondition = mongo::extension::host::extensionDispatcherInPlacePrecondition,
        .transform = Transforms::noop,
        .priority = kDefaultOptimizeInPlacePriority,
        .tags = PipelineRewriteContext::Tags::InPlace,
    },
    {
        .name = "EXTENSION_APPLY_PIPELINE_SUFFIX_DEPENDENCIES",
        .precondition = mongo::extension::host::extensionApplyDependenciesPrecondition,
        .transform = mongo::extension::host::extensionApplyDependenciesTransform,
        .priority = kDefaultOptimizeInPlacePriority + 1,
        .tags = PipelineRewriteContext::Tags::InPlace,
    });
REGISTER_RULES(DocumentSourceExtensionOptimizable,
               OPTIMIZE_AT_RULE(DocumentSourceExtensionOptimizable));
}  // namespace mongo::rule_based_rewrites::pipeline

// The DocsNeededBounds visitor function definitions and their registration live here rather than
// in docs_needed_bounds_visitor, to avoid a circular BUILD dependency:
//   docs_needed_bounds_visitor → extension_host → host_adapters → docs_needed_bounds_visitor
// Defining and registering from extension_host means only binaries that link extension_host (e.g.
// mongod) pay the typeinfo cost; binaries like dbtest that link docs_needed_bounds_visitor but not
// extension_host compile and link cleanly.
namespace mongo {
const ServiceContext::ConstructorActionRegisterer extensionDocsNeededBoundsVisitorRegisterer{
    "ExtensionDocsNeededBoundsVisitorRegisterer", [](ServiceContext* service) {
        auto& registry = getDocumentSourceVisitorRegistry(service);
        registry.registerVisitorFunc<DocsNeededBoundsContext,
                                     extension::host::DocumentSourceExtensionOptimizable>(
            &visit<DocsNeededBoundsContext, extension::host::DocumentSourceExtensionOptimizable>);
        registry.registerVisitorFunc<DocsNeededBoundsContext,
                                     extension::host::DocumentSourceExtensionForQueryShape>(
            &visit<DocsNeededBoundsContext, extension::host::DocumentSourceExtensionForQueryShape>);
    }};

namespace {
void applyDocsNeededBoundsEffect(DocsNeededBoundsContext* ctx,
                                 extension::MongoExtensionDocsNeededBoundsEffectEnum effect,
                                 boost::optional<std::int64_t> value) {
    using namespace mongo;
    // The IDL validator validateDocsNeededBoundsInfo guarantees that 'value' is set iff 'effect' is
    // kLimit or kSkip, so the dereferences below are safe.
    using Effect = extension::MongoExtensionDocsNeededBoundsEffectEnum;
    switch (effect) {
        case Effect::kUnknown:
            ctx->applyUnknownStage();
            return;
        case Effect::kBlocking:
            ctx->applyBlockingStage();
            return;
        case Effect::kPossibleDecrease:
            ctx->applyPossibleDecreaseStage();
            return;
        case Effect::kPossibleIncrease:
            ctx->applyPossibleIncreaseStage();
            return;
        case Effect::kNoEffect:
            return;
        case Effect::kLimit:
            ctx->applyLimit(*value);
            return;
        case Effect::kSkip:
            ctx->applySkip(*value);
            return;
    }
    MONGO_UNREACHABLE;
}
}  // namespace

void visitExtensionStage(DocsNeededBoundsContext* ctx,
                         const extension::host::DocumentSourceExtensionOptimizable& source) {
    auto boundsInfo = source.getDocsNeededBounds();
    if (!boundsInfo) {
        // Default to unknown bounds.
        ctx->applyUnknownStage();
        return;
    }

    applyDocsNeededBoundsEffect(ctx, boundsInfo->getEffect(), boundsInfo->getValue());
}
}  // namespace mongo
