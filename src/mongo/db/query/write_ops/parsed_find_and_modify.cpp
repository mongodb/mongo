// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/parsed_find_and_modify.h"

#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/write_component_serialization.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {

namespace parsed_find_and_modify_command {

StatusWith<ParsedFindAndModify> parse(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const write_ops::FindAndModifyCommandRequest* command,
    const UpdateRequest* updateRequest,
    std::unique_ptr<const ExtensionsCallback> extensionsCallback) {
    FindAndModifyOp::validateCommandRequest(*command);
    tassert(13742606,
            "Expected a non-remove findAndModify command for the update branch",
            !command->getRemove().value_or(false));

    auto parsedUpdate =
        parsed_update_command::parse(expCtx, updateRequest, std::move(extensionsCallback));
    if (!parsedUpdate.isOK()) {
        return parsedUpdate.getStatus();
    }
    expCtx->initializeReferencedSystemVariables();

    return ParsedFindAndModify(std::move(expCtx), command, std::move(parsedUpdate.getValue()));
}

StatusWith<ParsedFindAndModify> parse(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const write_ops::FindAndModifyCommandRequest* command,
    const DeleteRequest* deleteRequest,
    std::unique_ptr<const ExtensionsCallback> extensionsCallback) {
    FindAndModifyOp::validateCommandRequest(*command);
    tassert(13742607,
            "Expected a remove findAndModify command for the delete branch",
            command->getRemove().value_or(false));

    auto parsedDelete =
        parsed_delete_command::parse(expCtx, deleteRequest, std::move(extensionsCallback));
    if (!parsedDelete.isOK()) {
        return parsedDelete.getStatus();
    }
    expCtx->initializeReferencedSystemVariables();

    return ParsedFindAndModify(std::move(expCtx), command, std::move(parsedDelete.getValue()));
}

}  // namespace parsed_find_and_modify_command

bool ParsedFindAndModify::hasParsedFindCommand() const {
    return std::visit([](const auto& branch) { return branch.hasParsedFindCommand(); },
                      _parsedBranch);
}

BSONObj ParsedFindAndModify::serializeQuery(const query_shape::SerializationOptions& opts) const {
    return std::visit(
        [&](const auto& branch) { return query_shape::serializeWriteQuery(branch, opts); },
        _parsedBranch);
}

BSONObj ParsedFindAndModify::serializeSort(const query_shape::SerializationOptions& opts) const {
    return std::visit(
        [&](const auto& branch) {
            return query_shape::serializeFindAndModifySort(
                branch.parsedFind.get(), branch.getRequest()->getSort(), _expCtx, opts);
        },
        _parsedBranch);
}

BSONObj ParsedFindAndModify::serializeProjection(
    const query_shape::SerializationOptions& opts) const {
    if (!_projection && !_request->getFields().value_or(BSONObj()).isEmpty()) {
        auto projection = std::visit(
            [&](const auto& branch) {
                return query_shape::parseFindAndModifyProjection(branch.parsedFind.get(),
                                                                 branch.getRequest()->getQuery(),
                                                                 branch.getRequest()->getProj(),
                                                                 _expCtx,
                                                                 *branch.extensionsCallback);
            },
            _parsedBranch);
        // Projection parsing can reference system variables that the parsed query did not (for
        // example $$USER_ROLES in 'fields' only), so those are not yet initialized. Initialize
        // before caching so a failed initialization does not leave a cached projection that skips
        // it on the next call.
        _expCtx->initializeReferencedSystemVariables();
        _projection = std::make_unique<projection_ast::Projection>(std::move(projection));
    }
    return query_shape::serializeProjection(_projection.get(), opts);
}

Value ParsedFindAndModify::serializeUpdate(const query_shape::SerializationOptions& opts) const {
    tassert(13742603, "Cannot serialize an update for findAndModify remove", !isRemove());
    return query_shape::serializeUpdate(std::get<ParsedUpdate>(_parsedBranch), opts);
}

boost::optional<std::vector<BSONObj>> ParsedFindAndModify::serializeArrayFilters(
    const query_shape::SerializationOptions& opts) const {
    tassert(13742604, "Cannot serialize array filters for findAndModify remove", !isRemove());
    return query_shape::serializeArrayFilters(std::get<ParsedUpdate>(_parsedBranch), opts);
}
}  // namespace mongo
