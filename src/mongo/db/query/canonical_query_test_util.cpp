/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/canonical_query_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"

#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
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

std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(StringData queryStr) {
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
