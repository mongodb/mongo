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

#include "mongo/db/query/query_shape/update_cmd_shape.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"

#include <absl/hash/hash.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {
namespace {

BSONObj shapifyQuery(const ParsedUpdate& parsedUpdate, const SerializationOptions& opts) {
    tassert(11034203, "query is required to be parsed", parsedUpdate.hasParsedFindCommand());
    auto matchExpr = parsedUpdate.parsedFind->filter.get();
    return matchExpr ? matchExpr->serialize(opts) : BSONObj{};
}

Value shapifyUpdateOp(const ParsedUpdate& parsedUpdate,
                      const SerializationOptions& opts =
                          SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
    const auto modType = parsedUpdate.getRequest()->getUpdateModification().type();
    const auto* executor = parsedUpdate.getDriver()->getUpdateExecutor();
    switch (modType) {
        case write_ops::UpdateModification::Type::kReplacement:
            return opts.serializeLiteral(
                parsedUpdate.getRequest()->getUpdateModification().getUpdateReplacement());
        case write_ops::UpdateModification::Type::kPipeline:
            return Value(static_cast<const PipelineExecutor*>(executor)->serialize(opts));
        case write_ops::UpdateModification::Type::kModifier:
            return Value(static_cast<const UpdateTreeExecutor*>(executor)->serialize(opts));
        default:
            MONGO_UNREACHABLE_TASSERT(11034402);
    }
    return {};
}

boost::optional<BSONObj> shapifyUpdateConstants(const ParsedUpdate& parsedUpdate,
                                                const SerializationOptions& opts) {
    if (parsedUpdate.getDriver()->type() != UpdateDriver::UpdateType::kPipeline) {
        return boost::none;
    }

    const boost::optional<BSONObj>& constants = parsedUpdate.getRequest()->getUpdateConstants();
    if (!constants.has_value() || constants->isEmpty()) {
        return boost::none;
    }

    BSONObjBuilder shapifiedConstants;

    // Shapify each constant value, but keep variable names unchanged.
    for (const auto& elem : constants.value()) {
        StringData varName = elem.fieldNameStringData();
        Value shapifiedValue = opts.serializeLiteral(elem);
        shapifiedValue.addToBsonObj(&shapifiedConstants, varName);
    }

    return shapifiedConstants.obj();
}

}  // namespace

UpdateCmdShapeComponents::UpdateCmdShapeComponents(const ParsedUpdate& parsedUpdate,
                                                   LetShapeComponent let,
                                                   const SerializationOptions& opts)
    : representativeQ(shapifyQuery(parsedUpdate, opts)),
      _representativeUObj(shapifyUpdateOp(parsedUpdate, opts).wrap(""_sd)),
      representativeC(shapifyUpdateConstants(parsedUpdate, opts)),
      multi(parsedUpdate.getRequest()->getMulti()),
      upsert(parsedUpdate.getRequest()->isUpsert()),
      let(let) {
    // TODO(SERVER-113907): Support representativeArrayFilters when shapifying update modifiers.
}

void UpdateCmdShapeComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(
        std::move(state), simpleHash(representativeQ), simpleHash(_representativeUObj));
    state = absl::HashState::combine(
        std::move(state), representativeC.has_value(), representativeArrayFilters.has_value());
    if (representativeC) {
        state = absl::HashState::combine(std::move(state), simpleHash(*representativeC));
    }
    if (representativeArrayFilters) {
        // TODO(SERVER-113907): Revisit here when supporting representativeArrayFilters when
        // shapifying update modifiers.
        for (const auto& filter : *representativeArrayFilters) {
            state = absl::HashState::combine(std::move(state), simpleHash(filter));
        }
    }
    state = absl::HashState::combine(std::move(state), multi, upsert, let);
}

void UpdateCmdShapeComponents::appendTo(
    BSONObjBuilder& bob,
    const SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    bob.append("command", "update");

    bob.append(write_ops::UpdateOpEntry::kQFieldName, representativeQ);
    bob.appendAs(getRepresentativeU(), write_ops::UpdateOpEntry::kUFieldName);

    if (representativeC.has_value()) {
        bob.append(write_ops::UpdateOpEntry::kCFieldName, *representativeC);
    }
    if (representativeArrayFilters.has_value()) {
        bob.append(write_ops::UpdateOpEntry::kArrayFiltersFieldName, *representativeArrayFilters);
    }

    bob.append(write_ops::UpdateOpEntry::kMultiFieldName, bool(multi));
    bob.append(write_ops::UpdateOpEntry::kUpsertFieldName, bool(upsert));

    let.appendTo(bob, opts, expCtx);
}

// As part of the size, we must track the allocation of elements in the representative shapes.
size_t UpdateCmdShapeComponents::size() const {
    return sizeof(UpdateCmdShapeComponents) + representativeQ.objsize() +
        _representativeUObj.objsize() + (representativeC ? representativeC->objsize() : 0) +
        (representativeArrayFilters ? shape_helpers::containerSize(*representativeArrayFilters)
                                    : 0) +
        let.size() - sizeof(LetShapeComponent);
}

UpdateCmdShape::UpdateCmdShape(const write_ops::UpdateCommandRequest& updateCommand,
                               const ParsedUpdate& parsedUpdate,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(updateCommand.getNamespace(), parsedUpdate.getRequest()->getCollation().getOwned()),
      _components(parsedUpdate, LetShapeComponent(updateCommand.getLet(), expCtx)) {}

const CmdSpecificShapeComponents& UpdateCmdShape::specificComponents() const {
    return _components;
}

size_t UpdateCmdShape::extraSize() const {
    // To account for possible padding, we calculate the extra space with the difference instead of
    // using sizeof(bool);
    return sizeof(UpdateCmdShape) - sizeof(Shape) - sizeof(UpdateCmdShapeComponents);
}

void UpdateCmdShape::appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                                      OperationContext* opCtx,
                                                      const SerializationOptions& opts) const {
    tassert(11034200,
            "We don't support serializing to the unmodified shape here, since we have already "
            "shapified and stored the representative query - we've lost the original literals",
            !opts.isKeepingLiteralsUnchanged());

    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID, _components.let.shapifiedLet);
    if (opts == SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have this copy stored already!
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // Slow path: we need to re-parse from our representative shapes and re-shapify with 'opts'.

    // Prepare UpdateOpEntry and UpdateRequest to reconstruct ParsedUpdate.
    write_ops::UpdateOpEntry op;
    op.setQ(_components.representativeQ);
    op.setU(_components.getRepresentativeU());
    op.setC(_components.representativeC);
    op.setArrayFilters(_components.representativeArrayFilters);
    op.setMulti(_components.multi);
    op.setUpsert(_components.upsert);

    UpdateRequest updateRequest(op);
    tassert(11034202,
            "nssOrUUID for an update must be a namespace string",
            nssOrUUID.isNamespaceString());
    updateRequest.setNamespaceString(nssOrUUID.nss());
    if (_components.let.hasLet) {
        updateRequest.setLetParameters(_components.let.shapifiedLet);
    }

    auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
        expCtx,
        &updateRequest,
        makeExtensionsCallback<ExtensionsCallbackReal>(opCtx, &updateRequest.getNsString())));

    UpdateCmdShapeComponents{parsedUpdate, _components.let, opts}.appendTo(bob, opts, expCtx);
}

QueryShapeHash UpdateCmdShape::sha256Hash(OperationContext*, const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "update" command shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> updateCommandShapeBuffer;

    // Write small or typically empty "update" command shape parts to the buffer.
    updateCommandShapeBuffer.appendStrBytes(write_ops::UpdateCommandRequest::kCommandName);

    // Append bits corresponding to the optional multi and upsert fields, representativeC, and
    // representativeArrayFilters fields.
    updateCommandShapeBuffer.appendNum(
        static_cast<int>(_components.multi) << 3 | static_cast<int>(_components.upsert) << 2 |
        static_cast<int>(_components.representativeC.has_value()) << 1 |
        static_cast<int>(_components.representativeArrayFilters.has_value()));

    tassert(11183700,
            "nssOrUUID for an update must be a namespace string",
            nssOrUUID.isNamespaceString());
    auto nssDataRange = nssOrUUID.asDataRange();
    updateCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());
    updateCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());

    // TODO(SERVER-113907): Revisit here when supporting representativeArrayFilters when
    // shapifying update modifiers.
    // TODO: Because representativeArrayFilters is variable-sized, updateCommandShapeBuffer could
    // potentially exceed the stack allocation and require heap allocation. Consider using a
    // BSONArray instead of a vector of BSONObj and pass in asDataRange(...) to computeHash(...).
    if (_components.representativeArrayFilters) {
        const auto& filters = *_components.representativeArrayFilters;
        updateCommandShapeBuffer.appendNum(static_cast<uint32_t>(filters.size()));
        for (const auto& filter : filters) {
            updateCommandShapeBuffer.appendBuf(filter.objdata(), filter.objsize());
        }
    }

    BSONObj representativeC = _components.representativeC.value_or(BSONObj{});
    BSONObj representativeU = _components.getRepresentativeU().Obj();
    return SHA256Block::computeHash(
        {ConstDataRange{updateCommandShapeBuffer.buf(),
                        static_cast<std::size_t>(updateCommandShapeBuffer.len())},
         representativeU.asDataRange(),
         _components.representativeQ.asDataRange(),
         _components.let.shapifiedLet.asDataRange(),
         representativeC.asDataRange()});
}

}  // namespace mongo::query_shape
