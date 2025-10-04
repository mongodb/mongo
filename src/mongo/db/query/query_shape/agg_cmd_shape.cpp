/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/agg_cmd_shape.h"

#include "mongo/db/pipeline/expression_context_builder.h"
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

void AggCmdShape::appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                                   OperationContext* opCtx,
                                                   const SerializationOptions& opts) const {
    tassert(7633000,
            "We don't support serializing to the unmodified shape here, since we have already "
            "shapified and stored the representative query - we've lost the original literals",
            !opts.isKeepingLiteralsUnchanged());

    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID, _components.let.shapifiedLet);
    if (opts == SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have this copy stored already!
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // The cached pipeline shape doesn't match the requested options, so we have to
    // re-parse the pipeline from the initial request.
    expCtx->setInRouter(_inRouter);
    expCtx->addResolvedNamespaces(_components.involvedNamespaces);
    auto reparsed = Pipeline::parse(_components.representativePipeline, expCtx);
    auto serializedPipeline = reparsed->serializeToBson(opts);
    AggCmdShapeComponents{_components.allowDiskUse,
                          _components.involvedNamespaces,
                          serializedPipeline,
                          _components.let}
        .appendTo(bob, opts, expCtx);
}

void AggCmdShapeComponents::appendTo(BSONObjBuilder& bob,
                                     const SerializationOptions& opts,
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
