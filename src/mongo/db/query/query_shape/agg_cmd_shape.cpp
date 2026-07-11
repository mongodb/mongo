// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/agg_cmd_shape.h"

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/query_shape/shape_helpers.h"

namespace mongo::query_shape {

AggCmdShapeComponents::AggCmdShapeComponents(
    const AggregateCommandRequest& aggRequest,
    stdx::unordered_set<NamespaceString> involvedNamespaces_,
    std::vector<BSONObj> pipeline,
    LetShapeComponent let)
    : allowDiskUse(aggRequest.getAllowDiskUse()),
      involvedNamespaces(std::move(involvedNamespaces_)),
      representativePipeline(std::move(pipeline)),
      // Copying LetShapeComponent is safe, since the 'shapifiedLet' BSONObj is owned.
      let(let) {}

AggCmdShapeComponents::AggCmdShapeComponents(
    OptionalBool allowDiskUse,
    stdx::unordered_set<NamespaceString> involvedNamespaces_,
    std::vector<BSONObj> pipeline,
    LetShapeComponent let)
    : allowDiskUse(allowDiskUse),
      involvedNamespaces(std::move(involvedNamespaces_)),
      representativePipeline(std::move(pipeline)),
      // Copying LetShapeComponent is safe, since the 'shapifiedLet' BSONObj is owned.
      let(let) {}

void AggCmdShapeComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), allowDiskUse, let);
    for (auto&& shapifiedStage : representativePipeline) {
        state = absl::HashState::combine(std::move(state), simpleHash(shapifiedStage));
    }
}

void AggCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    tassert(7633000,
            "We don't support serializing to the unmodified shape here, since we have already "
            "shapified and stored the representative query - we've lost the original literals",
            !opts.isKeepingLiteralsUnchanged());

    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID, _components.let.shapifiedLet);
    if (opts == query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have this copy stored already!
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // The cached pipeline shape doesn't match the requested options, so we have to
    // re-parse the pipeline from the initial request.
    expCtx->setInRouter(_inRouter);
    expCtx->addResolvedNamespaces(_components.involvedNamespaces);
    auto reparsed = pipeline_factory::makePipeline(
        _components.representativePipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto serializedPipeline = reparsed->serializeToBson(opts);
    AggCmdShapeComponents{_components.allowDiskUse,
                          _components.involvedNamespaces,
                          serializedPipeline,
                          _components.let}
        .appendTo(bob, opts, expCtx);
}

void AggCmdShapeComponents::appendTo(BSONObjBuilder& bob,
                                     const query_shape::SerializationOptions& opts,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    let.appendTo(bob, opts, expCtx);

    bob.append("command", "aggregate");

    // pipeline
    bob.append(AggregateCommandRequest::kPipelineFieldName, representativePipeline);

    // allowDiskUse
    if (allowDiskUse.has_value()) {
        bob.append(AggregateCommandRequest::kAllowDiskUseFieldName, bool(allowDiskUse));
    }
}

// As part of the size, we must track the allocation of elements in the representative
// pipeline, as well as the elements in the unordered set of involved namespaces.
size_t AggCmdShapeComponents::size() const {
    return sizeof(AggCmdShapeComponents) + shape_helpers::containerSize(representativePipeline) +
        shape_helpers::containerSize(involvedNamespaces) + let.size() - sizeof(LetShapeComponent);
}

AggCmdShape::AggCmdShape(const AggregateCommandRequest& aggregateCommand,
                         NamespaceString origNss,
                         stdx::unordered_set<NamespaceString> involvedNamespaces,
                         const Pipeline& pipeline,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(std::move(origNss), aggregateCommand.getCollation().value_or(BSONObj())),
      _components(
          aggregateCommand,
          std::move(involvedNamespaces),
          pipeline.serializeToBson(SerializationOptions::kRepresentativeQueryShapeSerializeOptions),
          LetShapeComponent(aggregateCommand.getLet(), expCtx)),
      _inRouter(expCtx->getInRouter()) {}

const CmdSpecificShapeComponents& AggCmdShape::specificComponents() const {
    return _components;
}

size_t AggCmdShape::extraSize() const {
    // To account for possible padding, we calculate the extra space with the difference instead of
    // using sizeof(bool);
    return sizeof(AggCmdShape) - sizeof(Shape) - sizeof(AggCmdShapeComponents);
}

}  // namespace mongo::query_shape
