/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/query/count_request.h"

#include "mongo/db/query/query_request.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kCmdName[] = "count";
const char kQueryField[] = "query";
const char kLimitField[] = "limit";
const char kSkipField[] = "skip";
const char kHintField[] = "hint";
const char kCollationField[] = "collation";
const char kExplainField[] = "explain";
const char kCommentField[] = "comment";
const char kMaxTimeMSField[] = "maxTimeMS";
const char kReadConcernField[] = "readConcern";
}  // namespace

CountRequest::CountRequest(NamespaceString nss, BSONObj query)
    : _nss(std::move(nss)), _query(query.getOwned()) {}

void CountRequest::setHint(BSONObj hint) {
    _hint = hint.getOwned();
}

void CountRequest::setCollation(BSONObj collation) {
    _collation = collation.getOwned();
}

StatusWith<CountRequest> CountRequest::parseFromBSON(const NamespaceString& nss,
                                                     const BSONObj& cmdObj,
                                                     bool isExplain) {

    // We don't validate that "query" is a nested object due to SERVER-15456.
    CountRequest request(nss, cmdObj.getObjectField(kQueryField));

    // Limit
    if (cmdObj[kLimitField].isNumber()) {
        long long limit = cmdObj[kLimitField].numberLong();

        // For counts, limit and -limit mean the same thing.
        if (limit < 0) {
            limit = -limit;
        }

        request.setLimit(limit);
    } else if (cmdObj[kLimitField].ok()) {
        return Status(ErrorCodes::BadValue, "limit value is not a valid number");
    }

    // Skip
    if (cmdObj[kSkipField].isNumber()) {
        long long skip = cmdObj[kSkipField].numberLong();
        if (skip < 0) {
            return Status(ErrorCodes::BadValue, "skip value is negative in count query");
        }

        request.setSkip(skip);
    } else if (cmdObj[kSkipField].ok()) {
        return Status(ErrorCodes::BadValue, "skip value is not a valid number");
    }

    // maxTimeMS
    if (cmdObj[kMaxTimeMSField].ok()) {
        auto maxTimeMS = QueryRequest::parseMaxTimeMS(cmdObj[kMaxTimeMSField]);
        if (!maxTimeMS.isOK()) {
            return maxTimeMS.getStatus();
        }
        request.setMaxTimeMS(static_cast<unsigned int>(maxTimeMS.getValue()));
    }

    // Hint
    if (BSONType::Object == cmdObj[kHintField].type()) {
        request.setHint(cmdObj[kHintField].Obj());
    } else if (String == cmdObj[kHintField].type()) {
        const std::string hint = cmdObj.getStringField(kHintField);
        request.setHint(BSON("$hint" << hint));
    }

    // Collation
    if (BSONType::Object == cmdObj[kCollationField].type()) {
        request.setCollation(cmdObj[kCollationField].Obj());
    } else if (cmdObj[kCollationField].ok()) {
        return Status(ErrorCodes::BadValue, "collation value is not a document");
    }

    // readConcern
    if (BSONType::Object == cmdObj[kReadConcernField].type()) {
        request.setReadConcern(cmdObj[kReadConcernField].Obj());
    } else if (cmdObj[kReadConcernField].ok()) {
        return Status(ErrorCodes::BadValue, "readConcern value is not a document");
    }

    // unwrappedReadPref
    if (BSONType::Object == cmdObj[QueryRequest::kUnwrappedReadPrefField].type()) {
        request.setUnwrappedReadPref(cmdObj[QueryRequest::kUnwrappedReadPrefField].Obj());
    } else if (cmdObj[QueryRequest::kUnwrappedReadPrefField].ok()) {
        return Status(ErrorCodes::BadValue, "readPreference value is not a document");
    }

    // Comment
    if (BSONType::String == cmdObj[kCommentField].type()) {
        request.setComment(cmdObj[kCommentField].valueStringData());
    } else if (cmdObj[kCommentField].ok()) {
        return Status(ErrorCodes::BadValue, "comment value is not a string");
    }


    // Explain
    request.setExplain(isExplain);

    return request;
}

StatusWith<BSONObj> CountRequest::asAggregationCommand() const {
    BSONObjBuilder aggregationBuilder;
    aggregationBuilder.append("aggregate", _nss.coll());

    // Build an aggregation pipeline that performs the counting. We add stages that satisfy the
    // query, skip and limit before finishing with the actual $count stage.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    if (!_query.isEmpty()) {
        BSONObjBuilder matchBuilder(pipelineBuilder.subobjStart());
        matchBuilder.append("$match", _query);
        matchBuilder.doneFast();
    }
    if (_skip) {
        BSONObjBuilder skipBuilder(pipelineBuilder.subobjStart());
        skipBuilder.append("$skip", *_skip);
        skipBuilder.doneFast();
    }
    if (_limit) {
        BSONObjBuilder limitBuilder(pipelineBuilder.subobjStart());
        limitBuilder.append("$limit", *_limit);
        limitBuilder.doneFast();
    }

    BSONObjBuilder countBuilder(pipelineBuilder.subobjStart());
    countBuilder.append("$count", "count");
    countBuilder.doneFast();
    pipelineBuilder.doneFast();

    // Complete the command by appending the other options to count.
    if (_collation) {
        aggregationBuilder.append(kCollationField, *_collation);
    }

    if (_hint) {
        aggregationBuilder.append(kHintField, *_hint);
    }

    if (!_comment.empty()) {
        aggregationBuilder.append(kCommentField, _comment);
    }

    if (_maxTimeMS > 0) {
        aggregationBuilder.append(kMaxTimeMSField, _maxTimeMS);
    }

    if (!_readConcern.isEmpty()) {
        aggregationBuilder.append(kReadConcernField, _readConcern);
    }

    if (!_unwrappedReadPref.isEmpty()) {
        aggregationBuilder.append(QueryRequest::kUnwrappedReadPrefField, _unwrappedReadPref);
    }

    // The 'cursor' option is always specified so that aggregation uses the cursor interface.
    aggregationBuilder.append("cursor", BSONObj());

    return aggregationBuilder.obj();
}
}  // namespace mongo
