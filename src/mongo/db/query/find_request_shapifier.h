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

#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/request_shapifier.h"

namespace mongo::query_stats {

/**
 * Handles shapification for FindCommandRequests.
 */
class FindRequestShapifier final : public RequestShapifier {
public:
    FindRequestShapifier(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const ParsedFindCommand& request,
                         const boost::optional<std::string> applicationName = boost::none)
        : RequestShapifier(expCtx->opCtx, applicationName),
          _request(*request.findCommandRequest),
          _initialQueryStatsKey(makeQueryStatsKey(
              expCtx, request, SerializationOptions::kDefaultQueryShapeSerializeOptions)) {}

    BSONObj makeQueryStatsKey(const SerializationOptions& opts,
                              OperationContext* opCtx) const final;

    BSONObj makeQueryStatsKey(const SerializationOptions& opts,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx) const final;

private:
    BSONObj makeQueryStatsKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const ParsedFindCommand& parsedRequest,
                              const SerializationOptions& opts) const;

    boost::intrusive_ptr<ExpressionContext> makeDummyExpCtx(
        OperationContext* opCtx, const FindCommandRequest& request) const {
        auto expCtx = make_intrusive<ExpressionContext>(
            opCtx, request, nullptr /* collator doesn't matter here.*/, false /* mayDbProfile */);
        expCtx->maxFeatureCompatibilityVersion = boost::none;  // Ensure all features are allowed.
        // Expression counters are reported in serverStatus to indicate how often clients use
        // certain expressions/stages, so it's a side effect tied to parsing. We must stop
        // expression counters before re-parsing to avoid adding to the counters more than once per
        // a given query.
        expCtx->stopExpressionCounters();
        return expCtx;
    }

    FindCommandRequest _request;  // We make a copy of FindCommandRequest
                                  // since this instance may outlive the original request once
                                  // the RequestShapifier is moved to the query stats store.
    // This is computed and cached upon construction until asked for once - at which point this
    // transitions to boost::none. This both a performance and a memory optimization.
    //
    // On the performance side: we try to construct the query stats key by simply viewing the parse
    // trees that we would build normally (rather than re-parsing each piece ourselves). We
    // initialize this instance at the moment we have all the parsed AST pieces (e.g. the
    // MatchExpression tree for the filter) necessary to do this, before the regular command
    // processing path goes on to optimize or otherwise transform those pieces too much.
    //
    // On the memory side: we could just make a copy of each of the ASTs. But we chose to avoid
    // this due to a limited memory budget and since we need to store the backing BSON used to parse
    // the MatchExpression and other trees anyway - it would be redundant to copy everything here.
    // We'll just re-parse on demand when asked.
    mutable boost::optional<BSONObj> _initialQueryStatsKey;
};
}  // namespace mongo::query_stats
