/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/basic_types_gen.h"

namespace mongo {

template <typename T>
class StatusWith;
class Document;
class AggregateCommand;

namespace aggregation_request_helper {

/**
 * Helpers to serialize/deserialize AggregateCommand.
 */
static constexpr StringData kBatchSizeField = "batchSize"_sd;
static constexpr long long kDefaultBatchSize = 101;

/**
 * Create a new instance of AggregateCommand by parsing the raw command object. Returns a
 * non-OK status if a required field was missing, if there was an unrecognized field name or if
 * there was a bad value for one of the fields.
 *
 * If we are parsing a request for an explained aggregation with an explain verbosity provided,
 * then 'explainVerbosity' contains this information. In this case, 'cmdObj' may not itself
 * contain the explain specifier. Otherwise, 'explainVerbosity' should be boost::none.
 */
StatusWith<AggregateCommand> parseFromBSON(
    NamespaceString nss,
    const BSONObj& cmdObj,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity = boost::none);

/**
 * Convenience overload which constructs the request's NamespaceString from the given database
 * name and command object.
 */
StatusWith<AggregateCommand> parseFromBSON(
    const std::string& dbName,
    const BSONObj& cmdObj,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity = boost::none);

/*
 * The first field in 'cmdObj' must be a string representing a valid collection name, or the
 * number 1. In the latter case, returns a reserved namespace that does not represent a user
 * collection. See 'NamespaceString::makeCollectionlessAggregateNSS()'.
 */
NamespaceString parseNs(const std::string& dbname, const BSONObj& cmdObj);

/**
 * Serializes the options to a Document. Note that this serialization includes the original
 * pipeline object, as specified. Callers will likely want to override this field with a
 * serialization of a parsed and optimized Pipeline object.
 *
 * The explain option is not serialized. The preferred way to send an explain is with the explain
 * command, like: {explain: {aggregate: ...}, ...}, explain options are not part of the aggregate
 * command object.
 */
Document serializeToCommandDoc(const AggregateCommand& request);

BSONObj serializeToCommandObj(const AggregateCommand& request);

/**
 * Validate the aggregate command object.
 */
Status validate(const BSONObj& cmdObj, boost::optional<ExplainOptions::Verbosity> explainVerbosity);
}  // namespace aggregation_request_helper

/**
 * Custom serializers/deserializers for AggregateCommand.
 */

boost::optional<mongo::ExplainOptions::Verbosity> parseExplainModeFromBSON(
    const BSONElement& explainElem);

void serializeExplainToBSON(const mongo::ExplainOptions::Verbosity& explain,
                            StringData fieldName,
                            BSONObjBuilder* builder);

mongo::SimpleCursorOptions parseAggregateCursorFromBSON(const BSONElement& cursorElem);

void serializeAggregateCursorToBSON(const SimpleCursorOptions& cursor,
                                    StringData fieldName,
                                    BSONObjBuilder* builder);

/**
 * Parse an aggregation pipeline definition from 'pipelineElem'.
 */
static std::vector<BSONObj> parsePipelineFromBSON(const BSONElement& pipelineElem) {
    std::vector<BSONObj> pipeline;

    uassert(ErrorCodes::TypeMismatch,
            "'pipeline' option must be specified as an array",
            !pipelineElem.eoo() && pipelineElem.type() == BSONType::Array);

    for (auto elem : pipelineElem.Obj()) {
        uassert(ErrorCodes::TypeMismatch,
                "Each element of the 'pipeline' array must be an object",
                elem.type() == BSONType::Object);
        pipeline.push_back(elem.embeddedObject().getOwned());
    }

    return pipeline;
}
}  // namespace mongo
