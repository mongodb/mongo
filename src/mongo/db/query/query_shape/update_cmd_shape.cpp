// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/update_cmd_shape.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_shape/write_component_serialization.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"

#include <absl/hash/hash.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {
using namespace std::literals::string_view_literals;


UpdateCmdShapeComponents::UpdateCmdShapeComponents(const ParsedUpdate& parsedUpdate,
                                                   LetShapeComponent let,
                                                   const query_shape::SerializationOptions& opts)
    : representativeQ(serializeWriteQuery(parsedUpdate, opts)),
      _representativeUObj(serializeUpdate(parsedUpdate, opts).wrap(""sv)),
      representativeC(serializeUpdateConstants(parsedUpdate, opts)),
      representativeArrayFilters(serializeArrayFilters(parsedUpdate, opts)),
      multi(parsedUpdate.getRequest()->getMulti()),
      upsert(parsedUpdate.getRequest()->isUpsert()),
      let(let) {}

void UpdateCmdShapeComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(
        std::move(state), simpleHash(representativeQ), simpleHash(_representativeUObj));
    state = absl::HashState::combine(
        std::move(state), representativeC.has_value(), representativeArrayFilters.has_value());
    if (representativeC) {
        state = absl::HashState::combine(std::move(state), simpleHash(*representativeC));
    }
    if (representativeArrayFilters) {
        for (const auto& filter : *representativeArrayFilters) {
            state = absl::HashState::combine(std::move(state), simpleHash(filter));
        }
    }
    state = absl::HashState::combine(std::move(state), multi, upsert, let);
}

void UpdateCmdShapeComponents::appendTo(
    BSONObjBuilder& bob,
    const query_shape::SerializationOptions& opts,
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

UpdateCmdShape::UpdateCmdShape(const ParsedUpdate& parsedUpdate,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(parsedUpdate.getRequest()->getNamespaceString(),
            parsedUpdate.getRequest()->getCollation().getOwned()),
      _components(parsedUpdate,
                  LetShapeComponent(parsedUpdate.getRequest()->getLetParameters(), expCtx)) {}

const CmdSpecificShapeComponents& UpdateCmdShape::specificComponents() const {
    return _components;
}

size_t UpdateCmdShape::extraSize() const {
    // To account for possible padding, we calculate the extra space with the difference instead of
    // using sizeof(bool);
    return sizeof(UpdateCmdShape) - sizeof(Shape) - sizeof(UpdateCmdShapeComponents);
}

void UpdateCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    tassert(11034200,
            "We don't support serializing to the unmodified shape here, since we have already "
            "shapified and stored the representative query - we've lost the original literals",
            !opts.isKeepingLiteralsUnchanged());

    auto expCtx = makeBlankExpressionContext(opCtx, nssOrUUID, _components.let.shapifiedLet);
    if (opts == query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have this copy stored already!
        _components.appendTo(bob, opts, expCtx);
        return;
    }

    // Slow path: we need to re-parse from our representative shapes and re-shapify with 'opts'.
    expCtx->setIsReparsingRepresentativeQueryShape(true);

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
        expCtx, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

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
