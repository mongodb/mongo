// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

#include <memory>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DeleteRequest;
class UpdateRequest;

namespace query_shape {
struct SerializationOptions;
}  // namespace query_shape

class ParsedFindAndModify;

namespace parsed_find_and_modify_command {
StatusWith<ParsedFindAndModify> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                                      const write_ops::FindAndModifyCommandRequest* command,
                                      const UpdateRequest* updateRequest,
                                      std::unique_ptr<const ExtensionsCallback> extensionsCallback);

StatusWith<ParsedFindAndModify> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                                      const write_ops::FindAndModifyCommandRequest* command,
                                      const DeleteRequest* deleteRequest,
                                      std::unique_ptr<const ExtensionsCallback> extensionsCallback);
}  // namespace parsed_find_and_modify_command

/**
 * Parsed form of a findAndModify command.
 *
 * The update and remove branches compose the existing ParsedUpdate and ParsedDelete types,
 * respectively, parsed from the caller-provided write request.
 *
 * Like ParsedUpdate and ParsedDelete, this type borrows: it holds no BSON of its own. The caller
 * must keep the command and the write request alive for as long as this object - and anything
 * derived from it, such as a query shape - is used.
 */
class [[MONGO_MOD_PUBLIC]] ParsedFindAndModify {
public:
    ParsedFindAndModify(ParsedFindAndModify&&) = default;
    ParsedFindAndModify& operator=(ParsedFindAndModify&&) = delete;

    ParsedFindAndModify(const ParsedFindAndModify&) = delete;
    ParsedFindAndModify& operator=(const ParsedFindAndModify&) = delete;

    const write_ops::FindAndModifyCommandRequest& getRequest() const {
        return *_request;
    }

    bool isRemove() const {
        return std::holds_alternative<ParsedDelete>(_parsedBranch);
    }

    const ParsedUpdate* getParsedUpdate() const {
        return std::get_if<ParsedUpdate>(&_parsedBranch);
    }

    const ParsedDelete* getParsedDelete() const {
        return std::get_if<ParsedDelete>(&_parsedBranch);
    }

    bool hasParsedFindCommand() const;

    BSONObj serializeQuery(const query_shape::SerializationOptions& opts) const;

    BSONObj serializeSort(const query_shape::SerializationOptions& opts) const;

    BSONObj serializeProjection(const query_shape::SerializationOptions& opts) const;

    Value serializeUpdate(const query_shape::SerializationOptions& opts) const;

    boost::optional<std::vector<BSONObj>> serializeArrayFilters(
        const query_shape::SerializationOptions& opts) const;

private:
    ParsedFindAndModify(boost::intrusive_ptr<ExpressionContext> expCtx,
                        const write_ops::FindAndModifyCommandRequest* request,
                        std::variant<ParsedUpdate, ParsedDelete> parsedBranch)
        : _expCtx(std::move(expCtx)), _request(request), _parsedBranch(std::move(parsedBranch)) {}

    friend StatusWith<ParsedFindAndModify> parsed_find_and_modify_command::parse(
        boost::intrusive_ptr<ExpressionContext>,
        const write_ops::FindAndModifyCommandRequest*,
        const UpdateRequest*,
        std::unique_ptr<const ExtensionsCallback>);

    friend StatusWith<ParsedFindAndModify> parsed_find_and_modify_command::parse(
        boost::intrusive_ptr<ExpressionContext>,
        const write_ops::FindAndModifyCommandRequest*,
        const DeleteRequest*,
        std::unique_ptr<const ExtensionsCallback>);

    // The parsed branch may reference objects owned by the expression context (for example, a
    // collator held by a match expression), so the context is retained. It is declared before
    // '_request' and '_parsedBranch' so that it outlives both: members are destroyed in
    // reverse order.
    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // The findAndModify command. Borrowed: the caller must keep it alive.
    const write_ops::FindAndModifyCommandRequest* _request;

    // The parsed write branch. It borrows the caller's update/delete request, which the caller
    // must keep alive along with the command.
    std::variant<ParsedUpdate, ParsedDelete> _parsedBranch;

    // Returned-document projection is parsed lazily once for shape serialization. Keep this on
    // ParsedFindAndModify because the projection exists only for findAndModify and parsing it
    // repeatedly would charge the memory-intensive expression budget repeatedly. The AST may
    // retain pointers to the expression context's collator, which is retained above.
    mutable std::unique_ptr<projection_ast::Projection> _projection;
};

namespace parsed_find_and_modify_command {

/**
 * Parses the findAndModify command's update branch from the caller-provided update request. The
 * caller must configure the expression context for the command's namespace, let parameters,
 * collation, and runtime constants, as appropriate for its execution environment, and must keep
 * the command and the request alive for the lifetime of the returned ParsedFindAndModify.
 */
StatusWith<ParsedFindAndModify> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                                      const write_ops::FindAndModifyCommandRequest* command,
                                      const UpdateRequest* updateRequest,
                                      std::unique_ptr<const ExtensionsCallback> extensionsCallback);

/**
 * Parses the findAndModify command's remove branch from the caller-provided delete request. The
 * caller must configure the expression context for the command's namespace, let parameters,
 * collation, and runtime constants, as appropriate for its execution environment, and must keep
 * the command and the request alive for the lifetime of the returned ParsedFindAndModify.
 */
StatusWith<ParsedFindAndModify> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                                      const write_ops::FindAndModifyCommandRequest* command,
                                      const DeleteRequest* deleteRequest,
                                      std::unique_ptr<const ExtensionsCallback> extensionsCallback);

}  // namespace parsed_find_and_modify_command

}  // namespace mongo
