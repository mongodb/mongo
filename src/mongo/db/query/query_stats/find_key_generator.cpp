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

#include "mongo/db/query/query_stats/find_key_generator.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/query_shape.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

namespace mongo::query_stats {

std::unique_ptr<FindCommandRequest> FindKeyGenerator::reparse(OperationContext* opCtx) const {
    // TODO: SERVER-76330 factor out building the parseable cmdObj into a helper function in
    // query_shape.h.
    BSONObjBuilder cmdBuilder;
    NamespaceStringOrUUID nss = shape_helpers::parseNamespaceShape(_parseableQueryShape["cmdNs"]);
    nss.serialize(&cmdBuilder, FindCommandRequest::kCommandName);
    cmdBuilder.append("$db", DatabaseNameUtil::serialize(nss.dbName()));

    for (BSONElement e : _parseableQueryShape) {
        if (e.fieldNameStringData() == "cmdNs" || e.fieldNameStringData() == "command") {
            continue;
        }

        cmdBuilder.append(e);
    }

    auto cmdObj = cmdBuilder.obj();
    return std::make_unique<FindCommandRequest>(FindCommandRequest::parse(
        IDLParserContext("Query Stats Key", false /* apiStrict */, boost::none), cmdObj));
}

BSONObj FindKeyGenerator::generate(
    OperationContext* opCtx,
    boost::optional<SerializationOptions::TokenizeIdentifierFunc> hmacPolicy) const {
    auto request = reparse(opCtx);
    auto expCtx = makeDummyExpCtx(opCtx, *request);
    auto parsedRequest = uassertStatusOK(
        parsed_find_command::parse(expCtx,
                                   std::move(request),
                                   ExtensionsCallbackNoop(),
                                   MatchExpressionParser::kAllowAllSpecialFeatures));
    expCtx->setUserRoles();
    auto opts = hmacPolicy ? SerializationOptions{LiteralSerializationPolicy::kToDebugTypeString,
                                                  /*transformIdentifiersBool*/ true,
                                                  *hmacPolicy,
                                                  /*includePath*/ true,
                                                  /*verbosity*/ boost::none,
                                                  /*inMatchExprSortAndDedupElements*/ false}
                           : SerializationOptions{LiteralSerializationPolicy::kToDebugTypeString};

    return generateWithQueryShape(query_shape::extractQueryShape(*parsedRequest, opts, expCtx),
                                  opts);
}

void FindKeyGenerator::appendCommandSpecificComponents(BSONObjBuilder& bob,
                                                       const SerializationOptions& opts) const {
    if (_hasField.readConcern) {
        // Read concern should not be considered a literal.
        // afterClusterTime is distinct for every operation with causal consistency enabled. We
        // normalize it in order not to blow out the telemetry store cache.
        if (_readConcern["afterClusterTime"]) {
            BSONObjBuilder subObj = bob.subobjStart(FindCommandRequest::kReadConcernFieldName);

            if (auto levelElem = _readConcern["level"]) {
                subObj.append(levelElem);
            }
            opts.appendLiteral(&subObj, "afterClusterTime", _readConcern["afterClusterTime"]);
            subObj.doneFast();
        } else {
            bob.append(FindCommandRequest::kReadConcernFieldName, _readConcern);
        }
    }

    if (_hasField.allowPartialResults) {
        bob.append(FindCommandRequest::kAllowPartialResultsFieldName, _allowPartialResults);
    }

    // Fields for literal redaction. Adds batchSize, maxTimeMS, and noCursorTimeOut.

    if (_noCursorTimeout) {
        // Capture whether noCursorTimeout was specified in the query, do not distinguish between
        // true or false.
        opts.appendLiteral(
            &bob, FindCommandRequest::kNoCursorTimeoutFieldName, _hasField.noCursorTimeout);
    }

    if (_hasField.maxTimeMS) {
        opts.appendLiteral(&bob, FindCommandRequest::kMaxTimeMSFieldName, _maxTimeMS);
    }

    if (_hasField.batchSize) {
        opts.appendLiteral(
            &bob, FindCommandRequest::kBatchSizeFieldName, static_cast<long long>(_batchSize));
    }
}
}  // namespace mongo::query_stats
