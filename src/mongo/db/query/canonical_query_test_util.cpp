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

namespace mongo {
const NamespaceString CanonicalQueryTest::nss("test.collection");

/**
 * Utility functions to create a CanonicalQuery
 */
std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(const BSONObj& queryObj) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(queryObj);
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx(),
                                     std::move(findCommand),
                                     false,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

std::unique_ptr<CanonicalQuery> CanonicalQueryTest::canonicalize(StringData queryStr) {
    BSONObj queryObj = fromjson(queryStr.toString());
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
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx(),
                                     std::move(findCommand),
                                     false,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
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
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx(),
                                     std::move(findCommand),
                                     false,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
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
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx(),
                                     std::move(findCommand),
                                     explain,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

}  // namespace mongo
