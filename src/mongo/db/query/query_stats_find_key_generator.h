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

#include "mongo/bson/mutable/document.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_stats_key_generator.h"

namespace mongo::query_stats {

class FindKeyGenerator final : public KeyGenerator {
public:
    FindKeyGenerator(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     const ParsedFindCommand& request,
                     BSONObj parseableQueryShape,
                     boost::optional<StringData> collectionType = boost::none)
        : KeyGenerator(expCtx->opCtx, parseableQueryShape, collectionType),
          _readConcern(
              request.findCommandRequest->getReadConcern().has_value()
                  ? boost::optional<BSONObj>(request.findCommandRequest->getReadConcern()->copy())
                  : boost::none),
          _allowPartialResults(request.findCommandRequest->getAllowPartialResults()),
          _noCursorTimeout(request.findCommandRequest->getNoCursorTimeout()),
          _maxTimeMS(request.findCommandRequest->getMaxTimeMS()),
          _batchSize(request.findCommandRequest->getBatchSize()) {}


    BSONObj generate(OperationContext* opCtx,
                     boost::optional<SerializationOptions::TokenizeIdentifierFunc>) const final;

protected:
    int64_t doGetSize() const final {
        return sizeof(*this) + optionalObjSize(_readConcern);
    }

private:
    BSONObj makeQueryStatsKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const ParsedFindCommand& parsedRequest,
                              const SerializationOptions& opts) const;


    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const SerializationOptions& opts) const final override;

    std::unique_ptr<FindCommandRequest> reparse(OperationContext* opCtx) const;

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

    // Preserved literal.
    boost::optional<BSONObj> _readConcern;

    // Preserved literal.
    OptionalBool _allowPartialResults;

    // Shape.
    OptionalBool _noCursorTimeout;

    // Shape.
    boost::optional<int32_t> _maxTimeMS;

    // Shape.
    boost::optional<int64_t> _batchSize;
};
}  // namespace mongo::query_stats
