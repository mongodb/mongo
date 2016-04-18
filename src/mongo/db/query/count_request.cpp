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

#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kCmdName[] = "count";
const char kQueryField[] = "query";
const char kLimitField[] = "limit";
const char kSkipField[] = "skip";
const char kHintField[] = "hint";
const char kCollationField[] = "collation";

}  // namespace

CountRequest::CountRequest(NamespaceString nss, BSONObj query)
    : _nss(std::move(nss)), _query(query.getOwned()) {}

void CountRequest::setHint(BSONObj hint) {
    _hint = hint.getOwned();
}

void CountRequest::setCollation(BSONObj collation) {
    _collation = collation.getOwned();
}

BSONObj CountRequest::toBSON() const {
    BSONObjBuilder builder;

    builder.append(kCmdName, _nss.ns());
    builder.append(kQueryField, _query);

    if (_limit) {
        builder.append(kLimitField, _limit.get());
    }

    if (_skip) {
        builder.append(kSkipField, _skip.get());
    }

    if (_hint) {
        builder.append(kHintField, _hint.get());
    }

    if (_collation) {
        builder.append(kCollationField, _collation.get());
    }

    return builder.obj();
}

StatusWith<CountRequest> CountRequest::parseFromBSON(const std::string& dbname,
                                                     const BSONObj& cmdObj) {
    BSONElement firstElt = cmdObj.firstElement();
    const std::string coll = (firstElt.type() == BSONType::String) ? firstElt.str() : "";

    NamespaceString nss(dbname, coll);
    if (!nss.isValid()) {
        return Status(ErrorCodes::InvalidNamespace, "invalid collection name");
    }

    // We don't validate that "query" is a nested object due to SERVER-15456.
    CountRequest request(std::move(nss), cmdObj.getObjectField(kQueryField));

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

    // Hint
    if (Object == cmdObj[kHintField].type()) {
        request.setHint(cmdObj[kHintField].Obj());
    } else if (String == cmdObj[kHintField].type()) {
        const std::string hint = cmdObj.getStringField(kHintField);
        request.setHint(BSON("$hint" << hint));
    }

    // Collation
    if (Object == cmdObj[kCollationField].type()) {
        request.setCollation(cmdObj[kCollationField].Obj());
    } else if (cmdObj[kCollationField].ok()) {
        return Status(ErrorCodes::BadValue, "collation value is not a document");
    }

    return request;
}

}  // namespace mongo
