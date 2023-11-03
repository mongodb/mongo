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

#include "mongo/db/query/parsed_distinct_command.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_request_helper.h"

#include <iostream>

namespace mongo {

namespace {

std::unique_ptr<CollatorInterface> resolveCollator(OperationContext* opCtx,
                                                   const DistinctCommandRequest& distinct) {
    auto collation = distinct.getCollation();

    if (!collation || collation->isEmpty()) {
        return nullptr;
    }
    return uassertStatusOKWithContext(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(*collation),
        "unable to parse collation");
}

}  // namespace

namespace parsed_distinct_command {

std::unique_ptr<ParsedDistinctCommand> parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& cmd,
    std::unique_ptr<DistinctCommandRequest> distinctCommand,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures) {

    auto parsedDistinct = std::make_unique<ParsedDistinctCommand>();

    // Query.
    static BSONObj emptyQuery;
    auto query = distinctCommand->getQuery().get_value_or(emptyQuery);

    parsedDistinct->query = uassertStatusOK(
        MatchExpressionParser::parse(query, expCtx, extensionsCallback, allowedFeatures));

    // Collator.
    parsedDistinct->collator = resolveCollator(expCtx->opCtx, *distinctCommand);
    if (parsedDistinct->collator.get() && expCtx->getCollator()) {
        invariant(CollatorInterface::collatorsMatch(parsedDistinct->collator.get(),
                                                    expCtx->getCollator()));
    }

    // The IDL parser above does not handle generic command arguments. Since the underlying query
    // request requires the following options, manually parse and verify them here.

    // ReadConcern.
    if (auto readConcernElt = cmd[repl::ReadConcernArgs::kReadConcernFieldName]) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "\"" << repl::ReadConcernArgs::kReadConcernFieldName
                              << "\" had the wrong type. Expected " << typeName(BSONType::Object)
                              << ", found " << typeName(readConcernElt.type()),
                readConcernElt.type() == BSONType::Object);
        parsedDistinct->readConcern = readConcernElt.embeddedObject().getOwned();
    }

    // QueryOptions.
    if (auto queryOptionsElt = cmd[query_request_helper::kUnwrappedReadPrefField]) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "\"" << query_request_helper::kUnwrappedReadPrefField
                              << "\" had the wrong type. Expected " << typeName(BSONType::Object)
                              << ", found " << typeName(queryOptionsElt.type()),
                queryOptionsElt.type() == BSONType::Object);
        parsedDistinct->queryOptions = queryOptionsElt.embeddedObject().getOwned();
    }

    // MaxTimeMS.
    if (auto maxTimeMSElt = cmd[query_request_helper::cmdOptionMaxTimeMS]) {
        parsedDistinct->maxTimeMS = uassertStatusOK(parseMaxTimeMS(maxTimeMSElt));
    }

    // Rest of the command.
    parsedDistinct->distinctCommandRequest = std::move(distinctCommand);

    return parsedDistinct;
}

}  // namespace parsed_distinct_command
}  // namespace mongo
