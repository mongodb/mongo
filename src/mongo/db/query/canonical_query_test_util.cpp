// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/canonical_query_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"

#include <string_view>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
const NamespaceString CanonicalQueryTest::nss =
    NamespaceString::createNamespaceString_forTest("test.collection");

CanonicalQuery::QueryShapeString encodeKey(const CanonicalQuery& cq) {
    return (!cq.getExpCtx()->getQueryKnobConfiguration().isForceClassicEngineEnabled() &&
            cq.isSbeCompatible())
        ? canonical_query_encoder::encodeSBE(cq)
        : canonical_query_encoder::encodeClassic(cq);
}

/**
 * Utility functions to create a CanonicalQuery
 */
std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(const BSONObj& queryObj) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(queryObj);
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{
            .findCommand = std::move(findCommand),
            .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
}

std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(std::string_view queryStr) {
    BSONObj queryObj = fromjson(std::string{queryStr});
    return canonicalize(queryObj);
}

std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(BSONObj query,
                                                                 BSONObj sort,
                                                                 BSONObj proj,
                                                                 BSONObj collation) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    findCommand->setSort(sort);
    findCommand->setProjection(proj);
    findCommand->setCollation(collation);
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{
            .findCommand = std::move(findCommand),
            .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
}

std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(const char* queryStr,
                                                                 const char* sortStr,
                                                                 const char* projStr,
                                                                 const char* collationStr) {
    return canonicalize(
        fromjson(queryStr), fromjson(sortStr), fromjson(projStr), fromjson(collationStr));
}

std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(const char* queryStr,
                                                                 const char* sortStr,
                                                                 const char* projStr,
                                                                 long long skip,
                                                                 long long limit,
                                                                 const char* hintStr,
                                                                 const char* minStr,
                                                                 const char* maxStr) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));
    findCommand->setSort(fromjson(sortStr));
    findCommand->setProjection(fromjson(projStr));
    if (skip) {
        findCommand->setSkip(skip);
    }
    if (limit) {
        findCommand->setLimit(limit);
    }
    findCommand->setHint(fromjson(hintStr));
    findCommand->setMin(fromjson(minStr));
    findCommand->setMax(fromjson(maxStr));
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{
            .findCommand = std::move(findCommand),
            .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
}

std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(const char* queryStr,
                                                                 const char* sortStr,
                                                                 const char* projStr,
                                                                 long long skip,
                                                                 long long limit,
                                                                 const char* hintStr,
                                                                 const char* minStr,
                                                                 const char* maxStr,
                                                                 bool explain) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));
    findCommand->setSort(fromjson(sortStr));
    findCommand->setProjection(fromjson(projStr));
    if (skip) {
        findCommand->setSkip(skip);
    }
    if (limit) {
        findCommand->setLimit(limit);
    }
    findCommand->setHint(fromjson(hintStr));
    findCommand->setMin(fromjson(minStr));
    findCommand->setMax(fromjson(maxStr));

    boost::optional<ExplainOptions::Verbosity> verbosity =
        explain ? boost::make_optional(explain::VerbosityEnum::kQueryPlanner) : boost::none;
    auto expCtx =
        ExpressionContextBuilder{}.fromRequest(opCtx(), *findCommand).explain(verbosity).build();
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = std::move(expCtx),
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                              .allowedFeatures =
                                                  MatchExpressionParser::kAllowAllSpecialFeatures},
    });
}

}  // namespace mongo
