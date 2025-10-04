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

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_util.h"
#include "mongo/db/query/query_shape/shape_helpers.h"

namespace mongo::query_shape {
namespace {

// Compare the raw size of each class, ignoring any padding added from differences in size when
// compared with alignment.
static constexpr auto kExpectedAlignment =
    std::max(alignof(Shape), alignof(FindCmdShapeComponents));
static constexpr auto kExpectedPadding =
    (kExpectedAlignment - (sizeof(Shape) + sizeof(FindCmdShapeComponents)) % kExpectedAlignment) %
    kExpectedAlignment;
static_assert(
    sizeof(FindCmdShape) == sizeof(Shape) + sizeof(FindCmdShapeComponents) + kExpectedPadding,
    "If the class's members have changed, this assert and the extraSize() calculation may "
    "need to be updated with a new value.");

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
      let(request.findCommandRequest->getLet(), expCtx),
      hasField{.projection = request.proj.has_value(),
               .sort = request.sort.has_value(),
               .limit = request.findCommandRequest->getLimit().has_value(),
               .skip = request.findCommandRequest->getSkip().has_value()},
      serializationOpts(opts) {}

void FindCmdShapeComponents::appendTo(BSONObjBuilder& bob,
                                      const SerializationOptions& opts,
                                      const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    let.appendTo(bob, opts, expCtx);

    bob.append("command", "find");

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
            !serializationOpts.isKeepingLiteralsUnchanged());
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
                             let,
                             hasField);
}

uint32_t FindCmdShapeComponents::optionalArgumentsEncoding() const {
    uint32_t res{0};
    for (const auto& arg : {singleBatch,
                            allowDiskUse,
                            returnKey,
                            showRecordId,
                            tailable,
                            awaitData,
                            mirrored,
                            oplogReplay}) {
        if (arg.has_value()) {
            res |= arg ? 0b11 : 0b10;
        }
        res <<= 2;
    }

    res |= static_cast<uint32_t>(hasField.skip);
    res |= static_cast<uint32_t>(hasField.limit) << 1;

    return res;
}

const CmdSpecificShapeComponents& FindCmdShape::specificComponents() const {
    return _components;
}

std::unique_ptr<FindCommandRequest> FindCmdShape::toFindCommandRequest() const {
    auto fcr = std::make_unique<FindCommandRequest>(nssOrUUID);

    fcr->setFilter(_components.filter);
    if (_components.hasField.projection)
        fcr->setProjection(_components.projection);
    if (_components.hasField.sort)
        fcr->setSort(_components.sort);

    fcr->setMin(_components.min);
    fcr->setMax(_components.max);

    // Doesn't matter what value to use for limit and skip in the context of a shape.
    if (_components.hasField.limit)
        fcr->setLimit(1ll);
    if (_components.hasField.skip)
        fcr->setSkip(1ll);

    // All the booleans.
    if (_components.singleBatch.has_value())
        fcr->setSingleBatch(bool(_components.singleBatch));
    if (_components.allowDiskUse.has_value())
        fcr->setAllowDiskUse(bool(_components.allowDiskUse));
    if (_components.returnKey.has_value())
        fcr->setReturnKey(bool(_components.returnKey));
    if (_components.showRecordId.has_value())
        fcr->setShowRecordId(bool(_components.showRecordId));
    if (_components.tailable.has_value())
        fcr->setTailable(bool(_components.tailable));
    if (_components.awaitData.has_value())
        fcr->setAwaitData(bool(_components.awaitData));
    if (_components.mirrored.has_value())
        fcr->setMirrored(bool(_components.mirrored));
    if (_components.oplogReplay.has_value())
        fcr->setOplogReplay(bool(_components.oplogReplay));

    // Common shape components.
    if (_components.let.hasLet)
        fcr->setLet(_components.let.shapifiedLet);
    if (!collation.isEmpty())
        fcr->setCollation(collation);


    return fcr;
}

FindCmdShape::FindCmdShape(const ParsedFindCommand& findRequest,
                           const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(findRequest.findCommandRequest->getNamespaceOrUUID(),
            findRequest.findCommandRequest->getCollation()),
      _components(findRequest, expCtx) {}

void FindCmdShape::appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                                    OperationContext* opCtx,
                                                    const SerializationOptions& opts) const {
    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID, _components.let.shapifiedLet);
    if (opts == SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // Fast path: we already have this.
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // Slow path: we need to re-parse from our representative shapes.
    auto request = uassertStatusOKWithContext(
        parsed_find_command::parse(
            expCtx,
            {.findCommand = toFindCommandRequest(),
             .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}),
        "Could not re-parse a representative query shape");

    // This constructor will shapify according to the options.
    FindCmdShapeComponents{*request, expCtx, opts}.appendTo(bob, opts, expCtx);
}

QueryShapeHash FindCmdShape::sha256Hash(OperationContext*, const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "find" command shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> findCommandShapeBuffer;

    // Write small or typically empty "find" command shape parts to the buffer.
    findCommandShapeBuffer.appendStrBytes(FindCommandRequest::kCommandName);

    // Append bits corresponding to the optional command parameter values and a one bit indicator
    // whether the command specification includes a namespace or a UUID of a collection.
    findCommandShapeBuffer.appendNum(_components.optionalArgumentsEncoding() << 1 |
                                     (nssOrUUID.isNamespaceString() ? 1 : 0));
    auto nssDataRange = nssOrUUID.asDataRange();
    findCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());
    findCommandShapeBuffer.appendBuf(_components.min.objdata(), _components.min.objsize());
    findCommandShapeBuffer.appendBuf(_components.max.objdata(), _components.max.objsize());
    findCommandShapeBuffer.appendBuf(_components.sort.objdata(), _components.sort.objsize());
    findCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());
    findCommandShapeBuffer.appendBuf(_components.let.shapifiedLet.objdata(),
                                     _components.let.shapifiedLet.objsize());
    return SHA256Block::computeHash(
        {ConstDataRange{findCommandShapeBuffer.buf(),
                        static_cast<std::size_t>(findCommandShapeBuffer.len())},
         _components.filter.asDataRange(),
         _components.projection.asDataRange()});
}
}  // namespace mongo::query_shape
