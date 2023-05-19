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

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_policies.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo {

/**
 * Represents a find command request, but with more fully parsed ASTs for some fields which are
 * still raw BSONObj on the FindCommandRequest type.
 */
struct ParsedFindCommand {
    std::unique_ptr<CollatorInterface> collator;
    std::unique_ptr<MatchExpression> filter;
    boost::optional<projection_ast::Projection> proj;
    boost::optional<SortPattern> sort;

    // Based on parsing the query, which metadata will *not* be available. For example, if there is
    // no $text clause, then a text score will not be available.
    QueryMetadataBitSet unavailableMetadata;

    // This is saved for an edge case where we need to re-parse a projection later. Only populated
    // if there is a non-empty projection.
    boost::optional<ProjectionPolicies> savedProjectionPolicies;

    // All other parameters to the find command which do not have AST-like types and can be
    // appropriately tracked as raw value types like ints. The fields above like 'filter' are all
    // still present in their raw form on this FidnCommandRequest, but it is not expected that they
    // will be useful other than to keep the original BSON values around in-memory to avoid copying
    // large strings and such.
    std::unique_ptr<FindCommandRequest> findCommandRequest;
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
 * On success, returns a bitset indicating which types of metadata are *unavailable*. For example,
 * if 'root' does not contain a $text predicate, then the returned metadata bitset will indicate
 * that text score metadata is unavailable. This means that if subsequent $meta:"textScore"
 * expressions are found during analysis of the query, we should raise in an error.
 */
StatusWith<QueryMetadataBitSet> isValid(const MatchExpression* root,
                                        const FindCommandRequest& findCommand);


/**
 * Parses each big component of the input 'findCommand.' Throws exceptions if failing to parse.
 * Comes in one overload which will create an ExpressionContext for the caller, and one overload to
 * be used when the caller already has an ExpressionContext.
 */
StatusWith<std::pair<boost::intrusive_ptr<ExpressionContext>, std::unique_ptr<ParsedFindCommand>>>
parse(OperationContext* opCtx,
      std::unique_ptr<FindCommandRequest> findCommand,
      const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
      MatchExpressionParser::AllowedFeatureSet allowedFeatures =
          MatchExpressionParser::kDefaultSpecialFeatures,
      const ProjectionPolicies& projectionPolicies = ProjectionPolicies::findProjectionPolicies());

// Overload of the above for when the caller has an available ExpressionContext.
StatusWith<std::unique_ptr<ParsedFindCommand>> parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<FindCommandRequest> findCommand,
    const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
    MatchExpressionParser::AllowedFeatureSet allowedFeatures =
        MatchExpressionParser::kDefaultSpecialFeatures,
    const ProjectionPolicies& projectionPolicies = ProjectionPolicies::findProjectionPolicies());

}  // namespace parsed_find_command
}  // namespace mongo
