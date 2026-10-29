// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension::host {

/**
 * A DocumentSource implementation for pre-desugar extension stages. This object holds a parse node
 * for query shape serialization.
 */
class DocumentSourceExtensionForQueryShape : public DocumentSource {
public:
    static boost::intrusive_ptr<DocumentSourceExtensionForQueryShape> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONObj rawStage,
        AggStageDescriptorHandle staticDescriptor) {
        return boost::intrusive_ptr<DocumentSourceExtensionForQueryShape>(
            new DocumentSourceExtensionForQueryShape(expCtx, rawStage, staticDescriptor));
    }

    // Needed by the StageParams -> DS map. 'rawStage' is the original user-provided stage object,
    // used to round-trip literal-preserving serialization.
    static boost::intrusive_ptr<DocumentSourceExtensionForQueryShape> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        AggStageParseNodeHandle parseNode,
        BSONObj rawStage) {
        return boost::intrusive_ptr<DocumentSourceExtensionForQueryShape>(
            new DocumentSourceExtensionForQueryShape(
                expCtx, std::move(parseNode), std::move(rawStage)));
    }

    std::string_view getSourceName() const override {
        return _stageName;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    bool isUnexpandedDesugarPlaceholder() const override {
        return true;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    Value serialize(const query_shape::SerializationOptions& opts) const override;

    static const Id& id;

    Id getId() const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
        tasserted(
            11420600,
            "distributedPlanLogic() should not be called on "
            "DocumentSourceExtensionForQueryShape. Expandable stages should have been desugared "
            "to Optimizable stages before calling distributedPlanLogic()");
    }

protected:
    DocumentSourceExtensionForQueryShape(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         AggStageParseNodeHandle parseNode,
                                         BSONObj rawStage)
        : DocumentSource(parseNode->getName(), expCtx),
          _stageName(std::string(parseNode->getName())),
          _parseNode(std::move(parseNode)),
          _rawStage(rawStage.getOwned()) {}

private:
    const std::string _stageName;
    const AggStageParseNodeHandle _parseNode;
    // The original user-provided stage object, returned verbatim by literal-preserving
    // serialization (e.g. when a desugarer re-serializes a sub-pipeline for later re-parse).
    const BSONObj _rawStage;

    DocumentSourceExtensionForQueryShape(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         BSONObj rawStage,
                                         extension::AggStageDescriptorHandle staticDescriptor)
        : DocumentSource(staticDescriptor->getName(), expCtx),
          _stageName(std::string(staticDescriptor->getName())),
          _parseNode(staticDescriptor->parse(rawStage)),
          _rawStage(rawStage.getOwned()) {}
};

}  // namespace mongo::extension::host
