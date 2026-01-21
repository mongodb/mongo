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
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/vector_search_helper.h"
#include "mongo/util/assert_util.h"

namespace mongo::extension::host {

ALLOCATE_STAGE_PARAMS_ID(expandable, ExpandableStageParams::id);

ALLOCATE_STAGE_PARAMS_ID(expanded, ExpandedStageParams::id);

DocumentSourceContainer expandableStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* expandableParams = static_cast<ExpandableStageParams*>(stageParams.get());
    auto parseNode = expandableParams->releaseParseNode();

    if (expCtx->getFromRouter()) {
        // Because run_aggregate prevents a re-desugar when coming from mongos, it's necessary to
        // directly construct a DocumentSourceExtensionOptimizable from the parse node. This is
        // allowed because the desugar has been performed on the mongos already, meaning that the
        // BSON serialized and sent to the shards is from DocumentSourceExtensionOptimizable.
        return {DocumentSourceExtensionOptimizable::create(expCtx, std::move(parseNode))};
    }

    return {DocumentSourceExtensionForQueryShape::create(expCtx, std::move(parseNode))};
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
        tassert(10955800,
                str::stream() << "Stage expansion exceeded maximum depth of " << kMaxExpansionDepth,
                newDepth <= kMaxExpansionDepth);

        const auto inserted = _state.seenStages.insert(_stageName).second;
        tassert(10955801,
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
        constexpr StringData arrow = " -> "_sd;
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
        [&](const HostAggStageParseNode& hostParse) {
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
        [&](const HostAggStageAstNode& hostAst) {
            const auto& spec = hostAst.getIdLookupSpec();
            auto lpds = LiteParsedDocumentSource::parse(nss, spec, options);
            lpds->makeOwned();
            outExpanded.emplace_back(std::move(lpds));
        },
        [&](AggStageAstNodeHandle handle) {
            outExpanded.emplace_back(std::make_unique<LiteParsedExpanded>(
                std::string(handle->getName()), std::move(handle), nss));
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

// TODO SERVER-116021 Remove this check when the extension can do this through ViewPolicy.
bool DocumentSourceExtensionOptimizable::LiteParsedExpandable::isExtensionVectorSearchStage()
    const {
    return search_helpers::isExtensionVectorSearchStage(getParseTimeName());
}

// TODO SERVER-116021 Remove this check when the extension can do this through ViewPolicy.
bool DocumentSourceExtensionOptimizable::LiteParsedExpanded::isExtensionVectorSearchStage() const {
    return search_helpers::isExtensionVectorSearchStage(getParseTimeName());
}

ViewPolicy DocumentSourceExtensionOptimizable::LiteParsedExpanded::getViewPolicy() const {
    return ViewPolicy{.policy = view_util::toFirstStageApplicationPolicy(
                          _astNode->getFirstStageViewApplicationPolicy()),
                      .callback = [this](const ViewInfo& viewInfo, StringData stageName) {
                          const auto viewNameStr = NamespaceStringUtil::serialize(
                              viewInfo.viewName, SerializationContext::stateDefault());
                          _astNode->bindViewInfo(viewNameStr);
                      }};
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
         .allowedWithClientType = AllowedWithClientType::kAny});
}

ALLOCATE_DOCUMENT_SOURCE_ID(extensionOptimizable, DocumentSourceExtensionOptimizable::id);

Value DocumentSourceExtensionOptimizable::serialize(const SerializationOptions& opts) const {
    tassert(11217800,
            "SerializationOptions should keep literals unchanged while represented as a "
            "DocumentSourceExtensionOptimizable",
            opts.isKeepingLiteralsUnchanged());

    if (opts.isSerializingForExplain()) {
        return Value(_logicalStage->explain(*opts.verbosity));
    }

    // Serialize the stage for query execution.
    return Value(_logicalStage->serialize());
}

StageConstraints DocumentSourceExtensionOptimizable::constraints(
    PipelineSplitState pipeState) const {
    auto constraints = StageConstraints(StreamType::kStreaming,
                                        PositionRequirement::kNone,
                                        HostTypeRequirement::kNone,
                                        DiskUseRequirement::kNoDiskUse,
                                        FacetRequirement::kNotAllowed,
                                        TransactionRequirement::kNotAllowed,
                                        LookupRequirement::kNotAllowed,
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
    // TODO SERVER-117259 Enable extension stages in $lookup; change the default back to 'kAllowed'.
    // if (!_properties.getAllowedInLookup()) {
    //     constraints.lookupRequirement = StageConstraints::LookupRequirement::kNotAllowed;
    // }

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
    auto processFields = [](const auto& fields, auto&& apply) {
        if (fields.has_value()) {
            for (const auto& fieldName : *fields) {
                auto metaType = DocumentMetadataFields::parseMetaType(fieldName);
                apply(metaType);
            }
        }
    };

    // Report required metadata fields for this stage.
    processFields(_properties.getRequiredMetadataFields(),
                  [&](auto metaType) { deps->setNeedsMetadata(metaType); });

    // Drop upstream metadata fields if this stage does not preserve them.
    if (!_properties.getPreservesUpstreamMetadata()) {
        // TODO: SERVER-100443
        deps->clearMetadataAvailable();
    }

    // Report provided metadata fields for this stage.
    processFields(_properties.getProvidedMetadataFields(),
                  [&](auto metaType) { deps->setMetadataAvailable(metaType); });

    // Retain entire metadata and do not optimize, as it may be needed by the extension.
    return DepsTracker::State::NOT_SUPPORTED;
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
                    if (HostAggStageParseNode::isHostAllocated(*dplElement.get())) {
                        // Host-allocated: parse the host-allocated parse node.
                        const auto& hostParse =
                            *static_cast<const HostAggStageParseNode*>(dplElement.get());
                        return DocumentSource::parse(getExpCtx(), hostParse.getBsonSpec());
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
                    uassert(11513800,
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
        tassert(11420601,
                "Shards pipeline must have exactly one element per API specification",
                shardsPipeline.size() == 1);
        auto shardsStages = convertDPLHandleToDocumentSources(shardsPipeline[0]);
        tassert(11420602,
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
    }

    return logic;
}

boost::intrusive_ptr<DocumentSourceExtensionOptimizable> DocumentSourceExtensionOptimizable::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggStageParseNodeHandle& parseNodeHandle) {
    auto expanded = parseNodeHandle->expand();

    tassert(
        11623000, "Expected parseNode to only expand into a single node.", expanded.size() == 1);

    boost::intrusive_ptr<DocumentSourceExtensionOptimizable> optimizable = nullptr;
    helper::visitExpandedNodes(
        expanded,
        [&](const HostAggStageParseNode& host) {
            tasserted(11623001, "Expected extension AST node, got host parse node.");
        },
        [&](const AggStageParseNodeHandle& handle) {
            tasserted(11623002, "Expected extension AST node, got extension parse node.");
        },
        [&](const HostAggStageAstNode& hostAst) {
            tasserted(11623003, "Expected extension AST node, got host AST node.");
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
        [&](const HostAggStageParseNode& host) {
            const BSONObj& bsonSpec = host.getBsonSpec();
            outExpanded.splice(outExpanded.end(), DocumentSource::parse(expCtx, bsonSpec));
        },
        [&](const AggStageParseNodeHandle& handle) {
            std::list<boost::intrusive_ptr<DocumentSource>> children =
                expandParseNode(expCtx, handle);
            outExpanded.splice(outExpanded.end(), children);
        },
        [&](const HostAggStageAstNode& hostAst) {
            const BSONObj& bsonSpec = hostAst.getIdLookupSpec();
            outExpanded.splice(outExpanded.end(), DocumentSource::parse(expCtx, bsonSpec));
        },
        [&](AggStageAstNodeHandle handle) {
            outExpanded.emplace_back(
                DocumentSourceExtensionOptimizable::create(expCtx, std::move(handle)));
        });

    return outExpanded;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceExtensionOptimizable::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    return create(newExpCtx, _logicalStage->clone(), _properties);
}

DocumentSourceContainer::iterator DocumentSourceExtensionOptimizable::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // Attempt to remove a $sort on metadata if the extension stage is sorted by vector search
    // score.
    if (_logicalStage->isSortedByVectorSearchScore()) {
        if (auto result = search_helpers::applyVectorSearchSortOptimization(itr, container)) {
            return *result;
        }
    }
    return std::next(itr);
}
}  // namespace mongo::extension::host

namespace mongo::rule_based_rewrites::pipeline {
using DocumentSourceExtensionOptimizable =
    mongo::extension::host::DocumentSourceExtensionOptimizable;
REGISTER_RULES(DocumentSourceExtensionOptimizable,
               OPTIMIZE_AT_RULE(DocumentSourceExtensionOptimizable));

}  // namespace mongo::rule_based_rewrites::pipeline
