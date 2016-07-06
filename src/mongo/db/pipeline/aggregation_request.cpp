/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_request.h"

#include <algorithm>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

const StringData AggregationRequest::kCommandName = "aggregate"_sd;
const StringData AggregationRequest::kCursorName = "cursor"_sd;
const StringData AggregationRequest::kBatchSizeName = "batchSize"_sd;
const StringData AggregationRequest::kFromRouterName = "fromRouter"_sd;
const StringData AggregationRequest::kPipelineName = "pipeline"_sd;
const StringData AggregationRequest::kCollationName = "collation"_sd;
const StringData AggregationRequest::kExplainName = "explain"_sd;
const StringData AggregationRequest::kAllowDiskUseName = "allowDiskUse"_sd;

const long long AggregationRequest::kDefaultBatchSize = 101;

AggregationRequest::AggregationRequest(NamespaceString nss, std::vector<BSONObj> pipeline)
    : _nss(std::move(nss)), _pipeline(std::move(pipeline)) {}

StatusWith<AggregationRequest> AggregationRequest::parseFromBSON(NamespaceString nss,
                                                                 const BSONObj& cmdObj) {
    // Parse required parameters.
    auto pipelineElem = cmdObj[kPipelineName];
    if (pipelineElem.eoo() || pipelineElem.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch, "'pipeline' option must be specified as an array"};
    }
    std::vector<BSONObj> pipeline;
    for (auto elem : pipelineElem.Obj()) {
        if (elem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    "Each element of the 'pipeline' array must be an object"};
        }
        pipeline.push_back(elem.embeddedObject().getOwned());
    }

    AggregationRequest request(std::move(nss), std::move(pipeline));

    const std::initializer_list<StringData> optionsParsedElseWhere = {
        QueryRequest::cmdOptionMaxTimeMS,
        "writeConcern"_sd,
        kPipelineName,
        kCommandName,
        repl::ReadConcernArgs::kReadConcernFieldName};

    // Parse optional parameters.
    for (auto&& elem : cmdObj) {
        auto fieldName = elem.fieldNameStringData();

        // Ignore top-level fields prefixed with $. They are for the command processor, not us.
        if (fieldName[0] == '$') {
            continue;
        }

        // Ignore options that are parsed elsewhere.
        if (std::find(optionsParsedElseWhere.begin(), optionsParsedElseWhere.end(), fieldName) !=
            optionsParsedElseWhere.end()) {
            continue;
        }

        if (kCursorName == fieldName) {
            long long batchSize;
            auto status =
                CursorRequest::parseCommandCursorOptions(cmdObj, kDefaultBatchSize, &batchSize);
            if (!status.isOK()) {
                return status;
            }

            request.setCursorCommand(true);
            request.setBatchSize(batchSize);
        } else if (kCollationName == fieldName) {
            if (elem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << kCollationName << " must be an object, not a "
                                      << typeName(elem.type())};
            }
            request.setCollation(elem.embeddedObject().getOwned());
        } else if (kExplainName == fieldName) {
            if (elem.type() != BSONType::Bool) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << kExplainName << " must be a boolean, not a "
                                      << typeName(elem.type())};
            }
            request.setExplain(elem.Bool());
        } else if (kFromRouterName == fieldName) {
            if (elem.type() != BSONType::Bool) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << kFromRouterName << " must be a boolean, not a "
                                      << typeName(elem.type())};
            }
            request.setFromRouter(elem.Bool());
        } else if (kAllowDiskUseName == fieldName) {
            if (storageGlobalParams.readOnly) {
                return {ErrorCodes::IllegalOperation,
                        str::stream() << "The '" << kAllowDiskUseName
                                      << "' option is not permitted in read-only mode."};
            } else if (elem.type() != BSONType::Bool) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << kAllowDiskUseName << " must be a boolean, not a "
                                      << typeName(elem.type())};
            }
            request.setAllowDiskUse(elem.Bool());
        } else if (bypassDocumentValidationCommandOption() == fieldName) {
            request.setBypassDocumentValidation(elem.trueValue());
        } else {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "unrecognized field '" << elem.fieldName() << "'"};
        }
    }
    return request;
}

Document AggregationRequest::serializeToCommandObj() const {
    MutableDocument serialized;
    return Document{
        {kCommandName, _nss.coll()},
        {kPipelineName, _pipeline},
        // Only serialize booleans if different than their default.
        {kExplainName, _explain ? Value(true) : Value()},
        {kAllowDiskUseName, _allowDiskUse ? Value(true) : Value()},
        {kFromRouterName, _fromRouter ? Value(true) : Value()},
        {bypassDocumentValidationCommandOption(),
         _bypassDocumentValidation ? Value(true) : Value()},
        // Only serialize a collation if one was specified.
        {kCollationName, _collation.isEmpty() ? Value() : Value(_collation)},
        {kCursorName, _batchSize ? Value(Document{{kBatchSizeName, _batchSize.get()}}) : Value()}};
}

}  // namespace mongo
