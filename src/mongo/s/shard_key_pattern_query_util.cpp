/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/shard_key_pattern_query_util.h"

namespace mongo {

StatusWith<BSONObj> extractShardKeyFromBasicQuery(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const ShardKeyPattern& shardKeyPattern,
                                                  const BSONObj& basicQuery) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(basicQuery.getOwned());

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return shardKeyPattern.extractShardKeyFromQuery(*statusWithCQ.getValue());
}

StatusWith<BSONObj> extractShardKeyFromBasicQueryWithContext(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const ShardKeyPattern& shardKeyPattern,
    const BSONObj& basicQuery) {
    auto findCommand = std::make_unique<FindCommandRequest>(expCtx->ns);
    findCommand->setFilter(basicQuery.getOwned());
    if (!expCtx->getCollatorBSON().isEmpty()) {
        findCommand->setCollation(expCtx->getCollatorBSON().getOwned());
    }

    auto statusWithCQ =
        CanonicalQuery::canonicalize(expCtx->opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    return shardKeyPattern.extractShardKeyFromQuery(*statusWithCQ.getValue());
}

}  // namespace mongo
