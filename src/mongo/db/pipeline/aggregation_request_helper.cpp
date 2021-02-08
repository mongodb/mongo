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

#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/platform/basic.h"

namespace mongo {
namespace aggregation_request_helper {

/**
 * Validate the aggregate command object.
 */
void validate(const BSONObj& cmdObj, boost::optional<ExplainOptions::Verbosity> explainVerbosity);

AggregateCommand parseFromBSON(const std::string& dbName,
                               const BSONObj& cmdObj,
                               boost::optional<ExplainOptions::Verbosity> explainVerbosity,
                               bool apiStrict) {
    return parseFromBSON(parseNs(dbName, cmdObj), cmdObj, explainVerbosity, apiStrict);
}

StatusWith<AggregateCommand> parseFromBSONForTests(
    NamespaceString nss,
    const BSONObj& cmdObj,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    bool apiStrict) {
    try {
        return parseFromBSON(nss, cmdObj, explainVerbosity, apiStrict);
    } catch (const AssertionException&) {
        return exceptionToStatus();
    }
}

StatusWith<AggregateCommand> parseFromBSONForTests(
    const std::string& dbName,
    const BSONObj& cmdObj,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    bool apiStrict) {
    try {
        return parseFromBSON(dbName, cmdObj, explainVerbosity, apiStrict);
    } catch (const AssertionException&) {
        return exceptionToStatus();
    }
}

AggregateCommand parseFromBSON(NamespaceString nss,
                               const BSONObj& cmdObj,
                               boost::optional<ExplainOptions::Verbosity> explainVerbosity,
                               bool apiStrict) {

    // if the command object lacks field 'aggregate' or '$db', we will use the namespace in 'nss'.
    bool cmdObjChanged = false;
    auto cmdObjBob = BSONObjBuilder{BSON(AggregateCommand::kCommandName << nss.coll())};
    if (!cmdObj.hasField(AggregateCommand::kCommandName) ||
        !cmdObj.hasField(AggregateCommand::kDbNameFieldName)) {
        cmdObjBob.append("$db", nss.db());
        cmdObjBob.appendElementsUnique(cmdObj);
        cmdObjChanged = true;
    }

    AggregateCommand request(nss);
    request = AggregateCommand::parse(IDLParserErrorContext("aggregate", apiStrict),
                                      cmdObjChanged ? cmdObjBob.obj() : cmdObj);

    if (explainVerbosity) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "The '" << AggregateCommand::kExplainFieldName
                              << "' option is illegal when a explain verbosity is also provided",
                !cmdObj.hasField(AggregateCommand::kExplainFieldName));
        request.setExplain(explainVerbosity);
    }

    validate(cmdObj, explainVerbosity);

    return request;
}

NamespaceString parseNs(const std::string& dbname, const BSONObj& cmdObj) {
    auto firstElement = cmdObj.firstElement();

    if (firstElement.isNumber()) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Invalid command format: the '"
                              << firstElement.fieldNameStringData()
                              << "' field must specify a collection name or 1",
                firstElement.number() == 1);
        return NamespaceString::makeCollectionlessAggregateNSS(dbname);
    } else {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "collection name has invalid type: "
                              << typeName(firstElement.type()),
                firstElement.type() == BSONType::String);

        const NamespaceString nss(dbname, firstElement.valueStringData());

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
                nss.isValid() && !nss.isCollectionlessAggregateNS());

        return nss;
    }
}

BSONObj serializeToCommandObj(const AggregateCommand& request) {
    return request.toBSON(BSONObj());
}

Document serializeToCommandDoc(const AggregateCommand& request) {
    return Document(request.toBSON(BSONObj()).getOwned());
}

void validate(const BSONObj& cmdObj, boost::optional<ExplainOptions::Verbosity> explainVerbosity) {
    bool hasAllowDiskUseElem = cmdObj.hasField(AggregateCommand::kAllowDiskUseFieldName);
    bool hasCursorElem = cmdObj.hasField(AggregateCommand::kCursorFieldName);
    bool hasExplainElem = cmdObj.hasField(AggregateCommand::kExplainFieldName);
    bool hasExplain =
        explainVerbosity || (hasExplainElem && cmdObj[AggregateCommand::kExplainFieldName].Bool());
    bool hasFromMongosElem = cmdObj.hasField(AggregateCommand::kFromMongosFieldName);
    bool hasNeedsMergeElem = cmdObj.hasField(AggregateCommand::kNeedsMergeFieldName);

    // 'hasExplainElem' implies an aggregate command-level explain option, which does not require
    // a cursor argument.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The '" << AggregateCommand::kCursorFieldName
                          << "' option is required, except for aggregate with the explain argument",
            hasCursorElem || hasExplainElem);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Aggregation explain does not support the'"
                          << WriteConcernOptions::kWriteConcernField << "' option",
            !hasExplain || !cmdObj[WriteConcernOptions::kWriteConcernField]);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Cannot specify '" << AggregateCommand::kNeedsMergeFieldName
                          << "' without '" << AggregateCommand::kFromMongosFieldName << "'",
            (!hasNeedsMergeElem || hasFromMongosElem));

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "The '" << AggregateCommand::kAllowDiskUseFieldName
                          << "' option is not permitted in read-only mode.",
            (!hasAllowDiskUseElem || !storageGlobalParams.readOnly));
}
}  // namespace aggregation_request_helper

// Custom serializers/deserializers for AggregateCommand.

boost::optional<mongo::ExplainOptions::Verbosity> parseExplainModeFromBSON(
    const BSONElement& explainElem) {
    uassert(ErrorCodes::TypeMismatch,
            "explain must be a boolean",
            explainElem.type() == BSONType::Bool);

    if (explainElem.Bool()) {
        return ExplainOptions::Verbosity::kQueryPlanner;
    }

    return boost::none;
}

void serializeExplainToBSON(const mongo::ExplainOptions::Verbosity& explain,
                            StringData fieldName,
                            BSONObjBuilder* builder) {
    // Note that we do not serialize 'explain' field to the command object. This serializer only
    // serializes an empty cursor object for field 'cursor' when it is an explain command.
    builder->append(AggregateCommand::kCursorFieldName, BSONObj());

    return;
}

mongo::SimpleCursorOptions parseAggregateCursorFromBSON(const BSONElement& cursorElem) {
    if (cursorElem.eoo()) {
        SimpleCursorOptions cursor;
        cursor.setBatchSize(aggregation_request_helper::kDefaultBatchSize);
        return cursor;
    }

    uassert(ErrorCodes::TypeMismatch,
            "cursor field must be missing or an object",
            cursorElem.type() == mongo::Object);

    SimpleCursorOptions cursor = SimpleCursorOptions::parse(
        IDLParserErrorContext(AggregateCommand::kCursorFieldName), cursorElem.embeddedObject());
    if (!cursor.getBatchSize())
        cursor.setBatchSize(aggregation_request_helper::kDefaultBatchSize);

    return cursor;
}

void serializeAggregateCursorToBSON(const mongo::SimpleCursorOptions& cursor,
                                    StringData fieldName,
                                    BSONObjBuilder* builder) {
    if (!builder->hasField(fieldName)) {
        builder->append(
            fieldName,
            BSON(aggregation_request_helper::kBatchSizeField
                 << cursor.getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize)));
    }

    return;
}
}  // namespace mongo
