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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/collection_type.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_stats/key_generator.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::query_stats {

class FindKeyGenerator final : public KeyGenerator {
public:
    FindKeyGenerator(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const ParsedFindCommand& request,
        BSONObj parseableQueryShape,
        query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown)
        : KeyGenerator(expCtx->opCtx,
                       parseableQueryShape,
                       request.findCommandRequest->getHint(),
                       collectionType),
          _readConcern(request.findCommandRequest->getReadConcern().has_value()
                           ? request.findCommandRequest->getReadConcern()->copy()
                           : BSONObj()),
          _batchSize(request.findCommandRequest->getBatchSize().value_or(0)),
          _maxTimeMS(request.findCommandRequest->getMaxTimeMS().value_or(0)),
          _allowPartialResults(
              request.findCommandRequest->getAllowPartialResults().value_or(false)),
          _noCursorTimeout(request.findCommandRequest->getNoCursorTimeout().value_or(false)),
          _hasField{
              .readConcern = request.findCommandRequest->getReadConcern().has_value(),
              .batchSize = request.findCommandRequest->getBatchSize().has_value(),
              .maxTimeMS = request.findCommandRequest->getMaxTimeMS().has_value(),
              .allowPartialResults =
                  request.findCommandRequest->getAllowPartialResults().has_value(),
              .noCursorTimeout = request.findCommandRequest->getNoCursorTimeout().has_value(),
          } {}


    BSONObj generate(OperationContext* opCtx,
                     boost::optional<SerializationOptions::TokenizeIdentifierFunc>) const final;

protected:
    int64_t doGetSize() const final {
        return sizeof(*this) + (_hasField.readConcern ? _readConcern.objsize() : 0);
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

    // Avoid using boost::optional here because it creates extra padding at the beginning of the
    // struct. Since each QueryStatsEntry can have its own FindKeyGenerator, it's better to
    // minimize the struct's size as much as possible.

    // Preserved literal.
    BSONObj _readConcern;

    // Shape.
    int64_t _batchSize;

    // Shape.
    int32_t _maxTimeMS;

    // Preserved literal.
    bool _allowPartialResults;

    // Shape.
    bool _noCursorTimeout;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    struct {
        bool readConcern : 1 = false;
        bool batchSize : 1 = false;
        bool maxTimeMS : 1 = false;
        bool allowPartialResults : 1 = false;
        bool noCursorTimeout : 1 = false;
    } _hasField;
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(
    sizeof(FindKeyGenerator) <= sizeof(KeyGenerator) + sizeof(BSONObj) + 2 * sizeof(int64_t),
    "Size of FindKeyGenerator is too large! "
    "Make sure that the struct has been align- and padding-optimized. "
    "If the struct's members have changed, this assert may need to be updated with a new value.");
}  // namespace mongo::query_stats
