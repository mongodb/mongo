/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/delete_cmd_shape.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"

#include <absl/hash/hash.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {
namespace {

BSONObj shapifyQuery(const ParsedDelete& parsedDelete, const SerializationOptions& opts) {
    // Use the already-parsed query ('q' field) if we have it to avoid re-parsing. We won't have the
    // parsed query in the case where the 'q' field is a simple match on _id (e.g. {_id: 1}) - in
    // this case, we'll parse the query on-the-fly so we can shapify it.
    if (parsedDelete.hasParsedFindCommand()) {
        auto matchExpr = parsedDelete.parsedFind->filter.get();
        return matchExpr ? matchExpr->serialize(opts) : BSONObj{};
    }

    // Fast path for simple _id queries - we construct shape directly without parsing.
    const auto& query = parsedDelete.getRequest()->getQuery();
    BSONElement idElem = query["_id"];
    tassert(12205303, "Expected simple _id query", !idElem.eoo());
    dassert(isSimpleIdQuery(query));

    BSONElement valueElem = idElem;
    if (idElem.type() == BSONType::object &&
        idElem.Obj().firstElementFieldNameStringData() == "$eq"_sd) {
        valueElem = idElem.Obj().firstElement();
    }

    BSONObjBuilder result;
    BSONObjBuilder idObj(result.subobjStart(opts.serializeFieldPath("_id")));
    opts.appendLiteral(&idObj, "$eq", valueElem);
    idObj.doneFast();
    return result.obj();
}

}  // namespace

DeleteCmdShapeComponents::DeleteCmdShapeComponents(const ParsedDelete& parsedDelete,
                                                   LetShapeComponent let,
                                                   const SerializationOptions& opts)
    : representativeQ(shapifyQuery(parsedDelete, opts)),
      multi(parsedDelete.getRequest()->getMulti()),
      let(let) {}

void DeleteCmdShapeComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), simpleHash(representativeQ), multi, let);
}

void DeleteCmdShapeComponents::appendTo(
    BSONObjBuilder& bob,
    const SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    bob.append("command", "delete");

    bob.append(write_ops::DeleteOpEntry::kQFieldName, representativeQ);
    write_ops::writeMultiDeleteProperty(multi, write_ops::DeleteOpEntry::kMultiFieldName, &bob);

    let.appendTo(bob, opts, expCtx);
}

size_t DeleteCmdShapeComponents::size() const {
    return sizeof(DeleteCmdShapeComponents) + representativeQ.objsize() + let.size() -
        sizeof(LetShapeComponent);
}

DeleteCmdShape::DeleteCmdShape(const write_ops::DeleteCommandRequest& deleteCommand,
                               const ParsedDelete& parsedDelete,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(deleteCommand.getNamespace(), parsedDelete.getRequest()->getCollation().getOwned()),
      _components(parsedDelete, LetShapeComponent(deleteCommand.getLet(), expCtx)) {}

const CmdSpecificShapeComponents& DeleteCmdShape::specificComponents() const {
    return _components;
}

size_t DeleteCmdShape::extraSize() const {
    return sizeof(DeleteCmdShape) - sizeof(Shape) - sizeof(DeleteCmdShapeComponents);
}

void DeleteCmdShape::appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                                      OperationContext* opCtx,
                                                      const SerializationOptions& opts) const {
    tassert(12205300,
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

    DeleteRequest deleteRequest;
    tassert(12205301,
            "nssOrUUID for a delete must be a namespace string",
            nssOrUUID.isNamespaceString());
    deleteRequest.setNsString(nssOrUUID.nss());
    deleteRequest.setQuery(_components.representativeQ);
    deleteRequest.setMulti(_components.multi);
    if (!collation.isEmpty()) {
        deleteRequest.setCollation(collation.getOwned());
    }
    if (_components.let.hasLet) {
        deleteRequest.setLet(_components.let.shapifiedLet);
    }

    auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
        expCtx, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    DeleteCmdShapeComponents{parsedDelete, _components.let, opts}.appendTo(bob, opts, expCtx);
}

QueryShapeHash DeleteCmdShape::sha256Hash(OperationContext*, const SerializationContext&) const {
    // Allocate a buffer on the stack for serialization of parts of the "delete" command shape.
    constexpr std::size_t bufferSizeOnStack = 256;
    StackBufBuilderBase<bufferSizeOnStack> deleteCommandShapeBuffer;

    // Write small or typically empty "delete" command shape parts to the buffer.
    deleteCommandShapeBuffer.appendStrBytes(write_ops::DeleteCommandRequest::kCommandName);
    deleteCommandShapeBuffer.appendNum(static_cast<int>(_components.multi));

    tassert(12206000,
            "nssOrUUID for a delete must be a namespace string",
            nssOrUUID.isNamespaceString());
    auto nssDataRange = nssOrUUID.asDataRange();
    deleteCommandShapeBuffer.appendBuf(nssDataRange.data(), nssDataRange.length());

    deleteCommandShapeBuffer.appendBuf(collation.objdata(), collation.objsize());

    return SHA256Block::computeHash(
        {ConstDataRange{deleteCommandShapeBuffer.buf(),
                        static_cast<std::size_t>(deleteCommandShapeBuffer.len())},
         _components.representativeQ.asDataRange(),
         _components.let.shapifiedLet.asDataRange()});
}
}  // namespace mongo::query_shape
