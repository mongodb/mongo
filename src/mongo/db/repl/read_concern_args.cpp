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

#include "mongo/db/repl/read_concern_args.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

using std::string;

namespace mongo {
namespace repl {

namespace {

const char kLocalReadConcernStr[] = "local";
const char kMajorityReadConcernStr[] = "majority";
const char kLinearizableReadConcernStr[] = "linearizable";

}  // unnamed namespace

const string ReadConcernArgs::kReadConcernFieldName("readConcern");
const string ReadConcernArgs::kOpTimeFieldName("afterOpTime");
const string ReadConcernArgs::kOpTimestampFieldName("ts");
const string ReadConcernArgs::kOpTermFieldName("term");
const string ReadConcernArgs::kLevelFieldName("level");

ReadConcernArgs::ReadConcernArgs()
    : ReadConcernArgs(OpTime(), ReadConcernLevel::kLocalReadConcern) {}

ReadConcernArgs::ReadConcernArgs(OpTime opTime, ReadConcernLevel level)
    : _opTime(std::move(opTime)), _level(level) {}

ReadConcernLevel ReadConcernArgs::getLevel() const {
    return _level;
}

const OpTime& ReadConcernArgs::getOpTime() const {
    return _opTime;
}

Status ReadConcernArgs::initialize(const BSONObj& cmdObj) {
    auto readConcernElem = cmdObj[ReadConcernArgs::kReadConcernFieldName];

    if (readConcernElem.eoo()) {
        return Status::OK();
    }

    if (!readConcernElem.isABSONObj()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << kReadConcernFieldName << " field should be an object");
    }

    BSONObj readConcernObj = readConcernElem.Obj();
    BSONElement opTimeElem;
    auto opTimeStatus =
        bsonExtractTypedField(readConcernObj, kOpTimeFieldName, Object, &opTimeElem);

    if (opTimeStatus.isOK()) {
        BSONObj opTimeObj = opTimeElem.Obj();
        BSONElement timestampElem;

        Timestamp timestamp;
        auto timestampStatus =
            bsonExtractTimestampField(opTimeObj, kOpTimestampFieldName, &timestamp);

        if (!timestampStatus.isOK()) {
            return timestampStatus;
        }

        long long termNumber;
        auto termStatus = bsonExtractIntegerField(opTimeObj, kOpTermFieldName, &termNumber);

        if (!termStatus.isOK()) {
            return termStatus;
        }

        _opTime = OpTime(timestamp, termNumber);
    } else if (opTimeStatus != ErrorCodes::NoSuchKey) {
        return opTimeStatus;
    }

    std::string levelString;
    auto readCommittedStatus =
        bsonExtractStringField(readConcernObj, kLevelFieldName, &levelString);
    if (readCommittedStatus.isOK()) {
        if (levelString == kLocalReadConcernStr) {
            _level = ReadConcernLevel::kLocalReadConcern;
        } else if (levelString == kMajorityReadConcernStr) {
            _level = ReadConcernLevel::kMajorityReadConcern;
        } else {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << kReadConcernFieldName << '.' << kLevelFieldName
                                        << " must be either \"local\" or \"majority\"");
        }
    } else if (readCommittedStatus == ErrorCodes::NoSuchKey) {
        _level = ReadConcernLevel::kLocalReadConcern;
    } else {
        return readCommittedStatus;
    }

    return Status::OK();
}

void ReadConcernArgs::appendInfo(BSONObjBuilder* builder) {
    BSONObjBuilder rcBuilder(builder->subobjStart(kReadConcernFieldName));

    string levelName;
    switch (_level) {
        case ReadConcernLevel::kLocalReadConcern:
            levelName = kLocalReadConcernStr;
            break;

        case ReadConcernLevel::kMajorityReadConcern:
            levelName = kMajorityReadConcernStr;
            break;

        case ReadConcernLevel::kLinearizableReadConcern:
            levelName = kLinearizableReadConcernStr;
            break;

        default:
            fassert(28754, false);
    }

    rcBuilder.append(kLevelFieldName, levelName);

    BSONObjBuilder afterBuilder(rcBuilder.subobjStart(kOpTimeFieldName));
    afterBuilder.append(kOpTimestampFieldName, _opTime.getTimestamp());
    afterBuilder.append(kOpTermFieldName, _opTime.getTerm());
    afterBuilder.done();

    rcBuilder.done();
}

}  // namespace repl
}  // namespace mongo
