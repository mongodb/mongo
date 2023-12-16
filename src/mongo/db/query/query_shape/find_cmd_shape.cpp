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

#include "mongo/db/query/query_shape/find_cmd_shape.h"

#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/query_shape/shape_helpers.h"

namespace mongo::query_shape {
namespace {

BSONObj projectionShape(const boost::optional<projection_ast::Projection>& proj,
                        const SerializationOptions& opts =
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
    return proj ? projection_ast::serialize(*proj->root(), opts) : BSONObj();
}

BSONObj sortShape(const boost::optional<SortPattern>& sort,
                  const SerializationOptions& opts =
                      SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
    return sort
        ? sort->serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
              .toBson()
        : BSONObj();
}

void maybeAddWithName(const OptionalBool& optBool, BSONObjBuilder& bob, StringData name) {
    if (optBool.has_value()) {
        bob.append(name, bool(optBool));
    }
}

void addRemainingFindCommandFields(const FindCmdShapeComponents& components, BSONObjBuilder& bob) {
    maybeAddWithName(components.singleBatch, bob, FindCommandRequest::kSingleBatchFieldName);
    maybeAddWithName(components.allowDiskUse, bob, FindCommandRequest::kAllowDiskUseFieldName);
    maybeAddWithName(components.returnKey, bob, FindCommandRequest::kReturnKeyFieldName);
    maybeAddWithName(components.showRecordId, bob, FindCommandRequest::kShowRecordIdFieldName);
    maybeAddWithName(components.tailable, bob, FindCommandRequest::kTailableFieldName);
    maybeAddWithName(components.awaitData, bob, FindCommandRequest::kAwaitDataFieldName);
    maybeAddWithName(components.mirrored, bob, FindCommandRequest::kMirroredFieldName);
    maybeAddWithName(components.oplogReplay, bob, FindCommandRequest::kOplogReplayFieldName);
}

}  // namespace

FindCmdShapeComponents::FindCmdShapeComponents(
    const ParsedFindCommand& request,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SerializationOptions& opts)
    : filter(request.filter->serialize(opts)),
      projection(projectionShape(request.proj, opts)),
      sort(sortShape(request.sort, opts)),
      min(shape_helpers::extractMinOrMaxShape(request.findCommandRequest->getMin(), opts)),
      max(shape_helpers::extractMinOrMaxShape(request.findCommandRequest->getMax(), opts)),
      singleBatch(request.findCommandRequest->getSingleBatch()),
      allowDiskUse(request.findCommandRequest->getAllowDiskUse().has_value()
                       ? boost::optional<bool>(bool(request.findCommandRequest->getAllowDiskUse()))
                       : boost::none),
      returnKey(request.findCommandRequest->getReturnKey()),
      showRecordId(request.findCommandRequest->getShowRecordId()),
      tailable(request.findCommandRequest->getTailable()),
      awaitData(request.findCommandRequest->getAwaitData()),
      mirrored(request.findCommandRequest->getMirrored()),
      oplogReplay(request.findCommandRequest->getOplogReplay()),
      hasField{.projection = request.proj.has_value(),
               .sort = request.sort.has_value(),
               .limit = request.findCommandRequest->getLimit().has_value(),
               .skip = request.findCommandRequest->getSkip().has_value()},
      serializationOpts(opts) {}

void FindCmdShapeComponents::appendTo(BSONObjBuilder& bob) const {

    bob.append("command", "find");

    std::unique_ptr<MatchExpression> filterExpr;
    // Filter.
    bob.append(FindCommandRequest::kFilterFieldName, filter);

    if (hasField.projection) {
        bob.append(FindCommandRequest::kProjectionFieldName, projection);
    }

    if (!max.isEmpty()) {
        bob.append(FindCommandRequest::kMaxFieldName, max);
    }
    if (!min.isEmpty()) {
        bob.append(FindCommandRequest::kMinFieldName, min);
    }

    // Sort.
    if (hasField.sort) {
        bob.append(FindCommandRequest::kSortFieldName, sort);
    }

    // The values here don't matter (assuming we're not using the 'kUnchanged' policy).
    tassert(7973601,
            "Serialization policy not supported - original values have been discarded",
            serializationOpts.literalPolicy != LiteralSerializationPolicy::kUnchanged);
    if (hasField.limit) {
        serializationOpts.appendLiteral(&bob, FindCommandRequest::kLimitFieldName, 1ll);
    }
    if (hasField.skip) {
        serializationOpts.appendLiteral(&bob, FindCommandRequest::kSkipFieldName, 1ll);
    }

    // Add the fields that require no transformation.
    addRemainingFindCommandFields(*this, bob);
}

void FindCmdShapeComponents::HashValue(absl::HashState state) const {
    absl::HashState::combine(std::move(state),
                             simpleHash(filter),
                             simpleHash(projection),
                             simpleHash(sort),
                             simpleHash(min),
                             simpleHash(max),
                             singleBatch,
                             allowDiskUse,
                             returnKey,
                             showRecordId,
                             tailable,
                             awaitData,
                             mirrored,
                             oplogReplay,
                             hasField);
}

std::unique_ptr<FindCommandRequest> FindCmdShape::toFindCommandRequest() const {
    auto fcr = std::make_unique<FindCommandRequest>(nssOrUUID);

    fcr->setFilter(components.filter);
    if (components.hasField.projection)
        fcr->setProjection(components.projection);
    if (components.hasField.sort)
        fcr->setSort(components.sort);

    fcr->setMin(components.min);
    fcr->setMax(components.max);

    // Doesn't matter what value to use for limit and skip in the context of a shape.
    if (components.hasField.limit)
        fcr->setLimit(1ll);
    if (components.hasField.skip)
        fcr->setSkip(1ll);

    // All the booleans.
    if (components.singleBatch.has_value())
        fcr->setSingleBatch(bool(components.singleBatch));
    if (components.allowDiskUse.has_value())
        fcr->setAllowDiskUse(bool(components.allowDiskUse));
    if (components.returnKey.has_value())
        fcr->setReturnKey(bool(components.returnKey));
    if (components.showRecordId.has_value())
        fcr->setShowRecordId(bool(components.showRecordId));
    if (components.tailable.has_value())
        fcr->setTailable(bool(components.tailable));
    if (components.awaitData.has_value())
        fcr->setAwaitData(bool(components.awaitData));
    if (components.mirrored.has_value())
        fcr->setMirrored(bool(components.mirrored));
    if (components.oplogReplay.has_value())
        fcr->setOplogReplay(bool(components.oplogReplay));

    // Common shape components.
    if (_let.hasLet)
        fcr->setLet(_let.shapifiedLet);
    if (!collation.isEmpty())
        fcr->setCollation(collation);


    return fcr;
}

FindCmdShape::FindCmdShape(const ParsedFindCommand& findRequest,
                           const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : CmdWithLetShape(findRequest.findCommandRequest->getLet(),
                      expCtx,
                      components,
                      findRequest.findCommandRequest->getNamespaceOrUUID(),
                      findRequest.findCommandRequest->getCollation()),
      components(findRequest, expCtx) {}

void FindCmdShape::appendLetCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SerializationOptions& opts) const {
    if (opts == SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // Fast path: we already have this.
        return components.appendTo(bob);
    } else {
        // Slow path: we need to re-parse from our representative shapes.
        auto request = uassertStatusOKWithContext(
            parsed_find_command::parse(
                expCtx,
                {.findCommand = toFindCommandRequest(),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}),
            "Could not re-parse a representative query shape");

        // This constructor will shapify according to the options.
        FindCmdShapeComponents{*request, expCtx, opts}.appendTo(bob);
    }
}

}  // namespace mongo::query_shape
