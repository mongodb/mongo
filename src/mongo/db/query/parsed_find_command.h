// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

struct [[MONGO_MOD_PUBLIC]] ParsedFindCommandParams {
    std::unique_ptr<FindCommandRequest> findCommand;
    const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop();
    MatchExpressionParser::AllowedFeatureSet allowedFeatures =
        MatchExpressionParser::kDefaultSpecialFeatures;
    const ProjectionPolicies& projectionPolicies = ProjectionPolicies::findProjectionPolicies();
};

/**
 * Represents a find command request, but with more fully parsed ASTs for some fields which are
 * still raw BSONObj on the FindCommandRequest type.
 */
struct [[MONGO_MOD_PUBLIC]] ParsedFindCommand {
    ParsedFindCommand() = default;

    /**
     * This API adds the ability to construct from a pre-parsed filter. The other arguments will be
     * re-parsed again from BSON on the 'findCommandRequest' argument, since we don't have a good
     * way of cloning them.
     */
    static StatusWith<std::unique_ptr<ParsedFindCommand>> withExistingFilter(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<CollatorInterface> collator,
        std::unique_ptr<MatchExpression> filter,
        std::unique_ptr<FindCommandRequest> findCommandRequest,
        const ProjectionPolicies& projectionPolicies);

    std::unique_ptr<CollatorInterface> collator;
    std::unique_ptr<MatchExpression> filter;
    boost::optional<projection_ast::Projection> proj;
    boost::optional<SortPattern> sort;

    // Based on parsing the query, which metadata will be available. For example, if there is
    // a $text clause, then a text score will be available.
    QueryMetadataBitSet availableMetadata;

    // This is saved for an edge case where we need to re-parse a projection later. Only populated
    // if there is a non-empty projection.
    boost::optional<ProjectionPolicies> savedProjectionPolicies;

    // All other parameters to the find command which do not have AST-like types and can be
    // appropriately tracked as raw value types like ints. The fields above like 'filter' are all
    // still present in their raw form on this FindCommandRequest, but it is not expected that they
    // will be useful other than to keep the original BSON values around in-memory to avoid copying
    // large strings and such.
    std::unique_ptr<FindCommandRequest> findCommandRequest;

    inline BSONObj toBSON() const {
        return findCommandRequest->toBSON();
    }
};

namespace parsed_find_command {
/**
 * Validates the match expression 'root' as well as the query specified by 'request', checking for
 * illegal combinations of operators. Returns a non-OK status if any such illegal combination is
 * found.
 *
 * This method can be called both on normalized and non-normalized 'root'. However, some checks can
 * only be performed once the match expressions is normalized. To perform these checks one can call
 * 'CanonicalQuery::isValidNormalized()'.
 *
 * On success, returns a bitset indicating which types of metadata are available. For example,
 * if 'root' does not contain a $text predicate, then the returned metadata bitset will indicate
 * that text score metadata is not available. This means that if subsequent $meta:"textScore"
 * expressions are found during analysis of the query, we should raise in an error.
 */
StatusWith<QueryMetadataBitSet> validateAndGetAvailableMetadata(
    const MatchExpression* root, const FindCommandRequest& findCommand);

/**
 * Parses each big component of the input 'findCommand.' Throws exceptions if failing to parse.
 */
StatusWith<std::unique_ptr<ParsedFindCommand>> parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, ParsedFindCommandParams&& params);

/**
 * Converts the input 'countCommand' to a FindCommandRequest. Then, parses each big component of the
 * FindCommandRequest. Throws exception if fails to parse.
 */
StatusWith<std::unique_ptr<ParsedFindCommand>> parseFromCount(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CountCommandRequest& countCommand,
    const ExtensionsCallback& extensionsCallback,
    const NamespaceString& nss);
}  // namespace parsed_find_command
}  // namespace mongo
