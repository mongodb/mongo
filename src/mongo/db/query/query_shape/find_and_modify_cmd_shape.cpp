// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/find_and_modify_cmd_shape.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_shape/write_component_serialization.h"
#include "mongo/db/query/write_ops/delete_util.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/update/update_util.h"

#include <absl/hash/hash.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {

namespace {

NamespaceString branchRequestNss(const ParsedFindAndModify& parsed) {
    return parsed.isRemove() ? parsed.getParsedDelete()->getRequest()->getNsString()
                             : parsed.getParsedUpdate()->getRequest()->getNamespaceString();
}

BSONObj branchRequestCollation(const ParsedFindAndModify& parsed) {
    return parsed.isRemove() ? parsed.getParsedDelete()->getRequest()->getCollation().getOwned()
                             : parsed.getParsedUpdate()->getRequest()->getCollation().getOwned();
}

/**
 * Builds the shape components from the branch held by the parsed findAndModify. The branch is
 * chosen at runtime, so this cannot be a member-initializer ternary.
 */
FindAndModifyCmdShapeComponents makeComponents(
    const ParsedFindAndModify& parsed, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& command = parsed.getRequest();
    const auto* projection = parsed.projection();
    if (parsed.isRemove()) {
        const auto& request = *parsed.getParsedDelete()->getRequest();
        return {command,
                *parsed.getParsedDelete(),
                LetShapeComponent(request.getLet(), expCtx),
                expCtx,
                query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                projection};
    }
    const auto& request = *parsed.getParsedUpdate()->getRequest();
    return {command,
            *parsed.getParsedUpdate(),
            LetShapeComponent(request.getLetParameters(), expCtx),
            expCtx,
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
            projection};
}

}  // namespace

FindAndModifyCmdShapeComponents::FindAndModifyCmdShapeComponents(
    const write_ops::FindAndModifyCommandRequest& command,
    const ParsedUpdate& parsedUpdate,
    LetShapeComponent let,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SerializationOptions& opts,
    const projection_ast::Projection* cachedProjection)
    : representativeQuery(serializeWriteQuery(parsedUpdate, opts)),
      representativeSort(serializeFindAndModifySort(
          parsedUpdate.parsedFind.get(), parsedUpdate.getRequest()->getSort(), expCtx, opts)),
      representativeFields(
          cachedProjection ? serializeProjection(cachedProjection, opts)
                           : serializeFindAndModifyProjection(parsedUpdate.parsedFind.get(),
                                                              parsedUpdate.getRequest()->getQuery(),
                                                              parsedUpdate.getRequest()->getProj(),
                                                              expCtx,
                                                              *parsedUpdate.extensionsCallback,
                                                              opts)),
      _representativeUpdateObj(serializeUpdate(parsedUpdate, opts).wrap(""sv)),
      representativeArrayFilters(serializeArrayFilters(parsedUpdate, opts)),
      isRemove(false),
      upsert(command.getUpsert().value_or(false)),
      let(std::move(let)) {}

FindAndModifyCmdShapeComponents::FindAndModifyCmdShapeComponents(
    const write_ops::FindAndModifyCommandRequest& command,
    const ParsedDelete& parsedDelete,
    LetShapeComponent let,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SerializationOptions& opts,
    const projection_ast::Projection* cachedProjection)
    : representativeQuery(serializeWriteQuery(parsedDelete, opts)),
      representativeSort(serializeFindAndModifySort(
          parsedDelete.parsedFind.get(), parsedDelete.getRequest()->getSort(), expCtx, opts)),
      representativeFields(
          cachedProjection ? serializeProjection(cachedProjection, opts)
                           : serializeFindAndModifyProjection(parsedDelete.parsedFind.get(),
                                                              parsedDelete.getRequest()->getQuery(),
                                                              parsedDelete.getRequest()->getProj(),
                                                              expCtx,
                                                              *parsedDelete.extensionsCallback,
                                                              opts)),
      _representativeUpdateObj(BSONObj{}),
      representativeArrayFilters(boost::none),
      isRemove(true),
      upsert(false),
      let(std::move(let)) {}

void FindAndModifyCmdShapeComponents::appendTo(
    BSONObjBuilder& bob,
    const query_shape::SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    bob.append("command", "findAndModify");

    bob.append(write_ops::FindAndModifyCommandRequest::kQueryFieldName, representativeQuery);
    if (!representativeSort.isEmpty()) {
        bob.append(write_ops::FindAndModifyCommandRequest::kSortFieldName, representativeSort);
    }
    if (!representativeFields.isEmpty()) {
        bob.append(write_ops::FindAndModifyCommandRequest::kFieldsFieldName, representativeFields);
    }

    if (isRemove) {
        // A shapified boolean literal: representative output keeps 'true' (the representative
        // boolean), debug output redacts it to "?bool".
        static const BSONObj kRemoveTrueBacking = BSON("remove" << true);
        opts.appendLiteral(&bob,
                           write_ops::FindAndModifyCommandRequest::kRemoveFieldName,
                           kRemoveTrueBacking.firstElement());
    } else {
        bob.appendAs(getRepresentativeUpdate(),
                     write_ops::FindAndModifyCommandRequest::kUpdateFieldName);
        if (representativeArrayFilters) {
            bob.append(write_ops::FindAndModifyCommandRequest::kArrayFiltersFieldName,
                       *representativeArrayFilters);
        }
        // 'upsert' is kept unmodified in the shape rather than shapified, so that upsert: true
        // and upsert: false produce different shapes.
        bob.append(write_ops::FindAndModifyCommandRequest::kUpsertFieldName, upsert);
    }

    let.appendTo(bob, opts, expCtx);
}

// As part of the size, we must track the allocation of elements in the representative shapes.
size_t FindAndModifyCmdShapeComponents::size() const {
    return sizeof(FindAndModifyCmdShapeComponents) + representativeQuery.objsize() +
        representativeSort.objsize() + representativeFields.objsize() +
        _representativeUpdateObj.objsize() +
        (representativeArrayFilters ? shape_helpers::containerSize(*representativeArrayFilters)
                                    : 0) +
        let.size() - sizeof(LetShapeComponent);
}

void FindAndModifyCmdShapeComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state),
                                     simpleHash(representativeQuery),
                                     simpleHash(representativeSort),
                                     simpleHash(representativeFields),
                                     simpleHash(_representativeUpdateObj),
                                     isRemove,
                                     upsert);
    state = absl::HashState::combine(std::move(state), representativeArrayFilters.has_value());
    if (representativeArrayFilters) {
        for (const auto& filter : *representativeArrayFilters) {
            state = absl::HashState::combine(std::move(state), simpleHash(filter));
        }
    }
    state = absl::HashState::combine(std::move(state), let);
}

FindAndModifyCmdShape::FindAndModifyCmdShape(const write_ops::FindAndModifyCommandRequest& command,
                                             const ParsedUpdate& parsedUpdate,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(parsedUpdate.getRequest()->getNamespaceString(),
            parsedUpdate.getRequest()->getCollation().getOwned()),
      _components(command,
                  parsedUpdate,
                  LetShapeComponent(parsedUpdate.getRequest()->getLetParameters(), expCtx),
                  expCtx) {}

FindAndModifyCmdShape::FindAndModifyCmdShape(const write_ops::FindAndModifyCommandRequest& command,
                                             const ParsedDelete& parsedDelete,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(parsedDelete.getRequest()->getNsString(),
            parsedDelete.getRequest()->getCollation().getOwned()),
      _components(command,
                  parsedDelete,
                  LetShapeComponent(parsedDelete.getRequest()->getLet(), expCtx),
                  expCtx) {}

FindAndModifyCmdShape::FindAndModifyCmdShape(const ParsedFindAndModify& parsed,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Shape(branchRequestNss(parsed), branchRequestCollation(parsed)),
      _components(makeComponents(parsed, expCtx)) {}

const CmdSpecificShapeComponents& FindAndModifyCmdShape::specificComponents() const {
    return _components;
}

size_t FindAndModifyCmdShape::extraSize() const {
    // To account for possible padding, we calculate the extra space with the difference instead of
    // using sizeof(bool);
    return sizeof(FindAndModifyCmdShape) - sizeof(Shape) - sizeof(FindAndModifyCmdShapeComponents);
}

void FindAndModifyCmdShape::appendCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    OperationContext* opCtx,
    const query_shape::SerializationOptions& opts) const {
    tassert(13742800,
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

    tassert(13742801,
            "nssOrUUID for a findAndModify must be a namespace string",
            nssOrUUID.isNamespaceString());
    write_ops::FindAndModifyCommandRequest command(nssOrUUID.nss());
    command.setQuery(_components.representativeQuery);
    if (!_components.representativeSort.isEmpty()) {
        command.setSort(_components.representativeSort);
    }
    if (!_components.representativeFields.isEmpty()) {
        command.setFields(_components.representativeFields);
    }
    if (_components.let.hasLet) {
        command.setLet(_components.let.shapifiedLet);
    }

    // Reconstruct the branch from the semantic values rather than from the shapified output:
    // shapifying 'remove: true' produces the representative boolean, which would not identify the
    // branch when re-parsing.
    if (_components.isRemove) {
        command.setRemove(true);
    } else {
        command.setUpdate(write_ops::UpdateModification(_components.getRepresentativeUpdate()));
        if (_components.representativeArrayFilters) {
            command.setArrayFilters(*_components.representativeArrayFilters);
        }
        command.setUpsert(_components.upsert);
    }

    // Build the write request for the branch, exactly as the execution path would.
    UpdateRequest updateRequest;
    DeleteRequest deleteRequest;
    if (_components.isRemove) {
        deleteRequest.setNsString(command.getNamespace());
        delete_util::makeDeleteRequest(opCtx, command, /*explain=*/false, &deleteRequest);
    } else {
        updateRequest.setNamespaceString(command.getNamespace());
        update::makeUpdateRequest(opCtx, command, /*explain=*/boost::none, &updateRequest);
    }

    auto parsed = uassertStatusOK(_components.isRemove
                                      ? parsed_find_and_modify_command::parse(
                                            expCtx,
                                            &command,
                                            &deleteRequest,
                                            makeExtensionsCallback<ExtensionsCallbackNoop>())
                                      : parsed_find_and_modify_command::parse(
                                            expCtx,
                                            &command,
                                            &updateRequest,
                                            makeExtensionsCallback<ExtensionsCallbackNoop>()));

    FindAndModifyCmdShapeComponents components = _components.isRemove
        ? FindAndModifyCmdShapeComponents(command,
                                          *parsed.getParsedDelete(),
                                          _components.let,
                                          expCtx,
                                          opts,
                                          parsed.projection())
        : FindAndModifyCmdShapeComponents(command,
                                          *parsed.getParsedUpdate(),
                                          _components.let,
                                          expCtx,
                                          opts,
                                          parsed.projection());
    components.appendTo(bob, opts, expCtx);
}

}  // namespace mongo::query_shape
