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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/read_after_optime_args.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

using std::string;

namespace mongo {
namespace repl {

const string ReadAfterOpTimeArgs::kRootFieldName("$readConcern");
const string ReadAfterOpTimeArgs::kOpTimeFieldName("afterOpTime");
const string ReadAfterOpTimeArgs::kOpTimestampFieldName("ts");
const string ReadAfterOpTimeArgs::kOpTermFieldName("term");
const string ReadAfterOpTimeArgs::kReadCommittedFieldName("committed");

ReadAfterOpTimeArgs::ReadAfterOpTimeArgs() : ReadAfterOpTimeArgs(OpTime()) {}

ReadAfterOpTimeArgs::ReadAfterOpTimeArgs(OpTime opTime, bool readCommitted)
    : _opTime(std::move(opTime)), _isReadCommitted(readCommitted) {}

bool ReadAfterOpTimeArgs::isReadCommitted() const {
    return _isReadCommitted;
}

const OpTime& ReadAfterOpTimeArgs::getOpTime() const {
    return _opTime;
}

Status ReadAfterOpTimeArgs::initialize(const BSONObj& cmdObj) {
    auto afterElem = cmdObj[ReadAfterOpTimeArgs::kRootFieldName];

    if (afterElem.eoo()) {
        return Status::OK();
    }

    if (!afterElem.isABSONObj()) {
        return Status(ErrorCodes::FailedToParse, "'after' field should be an object");
    }

    BSONObj readAfterObj = afterElem.Obj();
    BSONElement opTimeElem;
    auto opTimeStatus = bsonExtractTypedField(
        readAfterObj, ReadAfterOpTimeArgs::kOpTimeFieldName, Object, &opTimeElem);

    if (!opTimeStatus.isOK()) {
        return opTimeStatus;
    }

    BSONObj opTimeObj = opTimeElem.Obj();
    BSONElement timestampElem;

    Timestamp timestamp;
    auto timestampStatus = bsonExtractTimestampField(
        opTimeObj, ReadAfterOpTimeArgs::kOpTimestampFieldName, &timestamp);

    if (!timestampStatus.isOK()) {
        return timestampStatus;
    }

    long long termNumber;
    auto termStatus =
        bsonExtractIntegerField(opTimeObj, ReadAfterOpTimeArgs::kOpTermFieldName, &termNumber);

    if (!termStatus.isOK()) {
        return termStatus;
    }

    _opTime = OpTime(timestamp, termNumber);

    auto readCommittedStatus = bsonExtractBooleanFieldWithDefault(
        cmdObj, ReadAfterOpTimeArgs::kReadCommittedFieldName, false, &_isReadCommitted);
    if (!readCommittedStatus.isOK()) {
        return readCommittedStatus;
    }

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
