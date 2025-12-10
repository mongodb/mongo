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

#include "mongo/db/extension/host/document_source_extension_expandable.h"

namespace mongo::extension::host {

ALLOCATE_DOCUMENT_SOURCE_ID(extensionOptimizable, DocumentSourceExtensionOptimizable::id);

Value DocumentSourceExtensionOptimizable::serialize(const SerializationOptions& opts) const {
    tassert(11217800,
            "SerializationOptions should keep literals unchanged while represented as a "
            "DocumentSourceExtensionOptimizable",
            opts.isKeepingLiteralsUnchanged());

    if (opts.isSerializingForExplain()) {
        return Value(_logicalStage.explain(*opts.verbosity));
    }

    // Serialize the stage for query execution.
    return Value(_logicalStage.serialize());
}

StageConstraints DocumentSourceExtensionOptimizable::constraints(
    PipelineSplitState pipeState) const {
    // Default properties if unset
    auto constraints = DocumentSourceExtension::constraints(pipeState);

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
DocumentSourceExtensionOptimizable::distributedPlanLogic() {
    auto dplHandle = _logicalStage.getDistributedPlanLogic();

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
                        return DocumentSourceExtensionExpandable::expandParseNode(getExpCtx(),
                                                                                  dplElement);
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
                            dplLogicalStage.getName() == _logicalStage.getName());
                    return std::list<boost::intrusive_ptr<DocumentSource>>{
                        DocumentSourceExtensionOptimizable::create(
                            getExpCtx(), std::move(dplLogicalStage), _properties)};
                }},
            handle);
    };

    DistributedPlanLogic logic;

    // Convert shardsPipeline.
    auto shardsPipeline = dplHandle.extractShardsPipeline();
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
    auto mergingPipeline = dplHandle.extractMergingPipeline();
    for (auto& handle : mergingPipeline) {
        auto stages = convertDPLHandleToDocumentSources(handle);
        logic.mergingStages.splice(logic.mergingStages.end(), stages);
    }

    // Convert sortPattern.
    const auto sortPattern = dplHandle.getSortPattern();
    if (!sortPattern.isEmpty()) {
        logic.mergeSortPattern = sortPattern.getOwned();
    }

    return logic;
}

}  // namespace mongo::extension::host
