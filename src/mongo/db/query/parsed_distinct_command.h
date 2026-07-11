// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Represents a distinct command request, but with more fully parsed ASTs for some fields which are
 * still raw BSONObj on the DistinctCommandRequest type.
 */
struct ParsedDistinctCommand {
    std::unique_ptr<CollatorInterface> collator;
    std::unique_ptr<MatchExpression> query;

    // All other parameters to the find command which do not have AST-like types and can be
    // appropriately tracked as raw value types like ints. The fields above like 'query' are all
    // still present in their raw form on this DistinctCommandRequest, but it is not expected that
    // they will be useful other than to keep the original BSON values around in-memory to avoid
    // copying large strings and such.
    std::unique_ptr<DistinctCommandRequest> distinctCommandRequest;

    inline BSONObj toBSON() const {
        return distinctCommandRequest->toBSON();
    }
};

namespace parsed_distinct_command {
/**
 * Creates a projection spec for a distinct command from the requested field. In most cases, the
 * projection spec will be {_id: 0, key: 1}.
 * The exceptions are:
 * 1) When the requested field is '_id', the projection spec will {_id: 1}.
 * 2) When the requested field could be an array element (eg. a.0), the projected field will be the
 *    prefix of the field up to the array element. For example, a.b.2 => {_id: 0, 'a.b': 1} Note
 *    that we can't use a $slice projection because the distinct command filters the results from
 *    the executor using the dotted field name. Using $slice will re-order the documents in the
 *    array in the results.
 */
BSONObj getDistinctProjection(const std::string& field);

/**
 * Parses each big component of the input 'distinctCommand'.
 *
 * 'extensionsCallback' allows for additional mongod parsing. If called from mongos, an
 * ExtensionsCallbackNoop object should be passed to skip this parsing.
 */
std::unique_ptr<ParsedDistinctCommand> parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<DistinctCommandRequest> distinctCommand,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures);

/**
 * Convert the canonical query with a distinct field into an aggregation command request.
 */
AggregateCommandRequest asAggregation(const CanonicalQuery& query,
                                      boost::optional<ExplainOptions::Verbosity> verbosity,
                                      const SerializationContext& serializationContext);

/**
 * Convert the parsed distinct command into a canonical query with a distinct field.
 */
std::unique_ptr<CanonicalQuery> parseCanonicalQuery(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<ParsedDistinctCommand> parsedDistinct,
    const CollatorInterface* defaultCollator = nullptr);

}  // namespace parsed_distinct_command
}  // namespace mongo
