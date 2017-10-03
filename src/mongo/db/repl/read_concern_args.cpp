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
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/util/mongoutils/str.h"

using std::string;

namespace mongo {
namespace repl {

namespace {

const char kLocalReadConcernStr[] = "local";
const char kMajorityReadConcernStr[] = "majority";
const char kLinearizableReadConcernStr[] = "linearizable";
const char kAvailableReadConcernStr[] = "available";

}  // unnamed namespace

const string ReadConcernArgs::kReadConcernFieldName("readConcern");
const string ReadConcernArgs::kAfterOpTimeFieldName("afterOpTime");
const string ReadConcernArgs::kAfterClusterTimeFieldName("afterClusterTime");
const string ReadConcernArgs::kAtClusterTimeFieldName("atClusterTime");

const string ReadConcernArgs::kLevelFieldName("level");

const OperationContext::Decoration<ReadConcernArgs> ReadConcernArgs::get =
    OperationContext::declareDecoration<ReadConcernArgs>();

ReadConcernArgs::ReadConcernArgs() = default;

ReadConcernArgs::ReadConcernArgs(boost::optional<ReadConcernLevel> level)
    : _level(std::move(level)) {}

ReadConcernArgs::ReadConcernArgs(boost::optional<OpTime> opTime,
                                 boost::optional<ReadConcernLevel> level)
    : _opTime(std::move(opTime)), _level(std::move(level)) {}

ReadConcernArgs::ReadConcernArgs(boost::optional<LogicalTime> clusterTime,
                                 boost::optional<ReadConcernLevel> level)
    : _clusterTime(std::move(clusterTime)), _level(std::move(level)) {}

std::string ReadConcernArgs::toString() const {
    return toBSON().toString();
}

BSONObj ReadConcernArgs::toBSON() const {
    BSONObjBuilder bob;
    appendInfo(&bob);
    return bob.obj();
}

bool ReadConcernArgs::isEmpty() const {
    return !_clusterTime && !_opTime && !_level;
}

ReadConcernLevel ReadConcernArgs::getLevel() const {
    return _level.value_or(ReadConcernLevel::kLocalReadConcern);
}

bool ReadConcernArgs::hasLevel() const {
    return _level.is_initialized();
}

boost::optional<OpTime> ReadConcernArgs::getArgsOpTime() const {
    return _opTime;
}

boost::optional<LogicalTime> ReadConcernArgs::getArgsClusterTime() const {
    return _clusterTime;
}

boost::optional<LogicalTime> ReadConcernArgs::getArgsPointInTime() const {
    return _pointInTime;
}

Status ReadConcernArgs::initialize(const BSONElement& readConcernElem, bool testMode) {
    invariant(isEmpty());  // only legal to call on uninitialized object.

    if (readConcernElem.eoo()) {
        return Status::OK();
    }

    dassert(readConcernElem.fieldNameStringData() == kReadConcernFieldName);

    if (readConcernElem.type() != Object) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << kReadConcernFieldName << " field should be an object");
    }

    BSONObj readConcernObj = readConcernElem.Obj();
    for (auto&& field : readConcernObj) {
        auto fieldName = field.fieldNameStringData();
        if (fieldName == kAfterOpTimeFieldName) {
            OpTime opTime;
            // TODO pass field in rather than scanning again.
            auto opTimeStatus =
                bsonExtractOpTimeField(readConcernObj, kAfterOpTimeFieldName, &opTime);
            if (!opTimeStatus.isOK()) {
                return opTimeStatus;
            }
            _opTime = opTime;
        } else if (fieldName == kAfterClusterTimeFieldName) {
            Timestamp clusterTime;
            auto clusterTimeStatus =
                bsonExtractTimestampField(readConcernObj, kAfterClusterTimeFieldName, &clusterTime);
            if (!clusterTimeStatus.isOK()) {
                return clusterTimeStatus;
            }
            _clusterTime = LogicalTime(clusterTime);
        } else if (fieldName == kAtClusterTimeFieldName && testMode) {
            Timestamp pointInTime;
            auto pointInTimeStatus =
                bsonExtractTimestampField(readConcernObj, kAtClusterTimeFieldName, &pointInTime);
            if (!pointInTimeStatus.isOK()) {
                return pointInTimeStatus;
            }
            _pointInTime = LogicalTime(pointInTime);
        } else if (fieldName == kLevelFieldName) {
            std::string levelString;
            // TODO pass field in rather than scanning again.
            auto readCommittedStatus =
                bsonExtractStringField(readConcernObj, kLevelFieldName, &levelString);

            if (!readCommittedStatus.isOK()) {
                return readCommittedStatus;
            }

            if (levelString == kLocalReadConcernStr) {
                _level = ReadConcernLevel::kLocalReadConcern;
            } else if (levelString == kMajorityReadConcernStr) {
                _level = ReadConcernLevel::kMajorityReadConcern;
            } else if (levelString == kLinearizableReadConcernStr) {
                _level = ReadConcernLevel::kLinearizableReadConcern;
            } else if (levelString == kAvailableReadConcernStr) {
                _level = ReadConcernLevel::kAvailableReadConcern;
            } else {
                return Status(
                    ErrorCodes::FailedToParse,
                    str::stream()
                        << kReadConcernFieldName
                        << '.'
                        << kLevelFieldName
                        << " must be either 'local', 'majority', 'linearizable', or 'available'");
            }
        } else {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "Unrecognized option in " << kReadConcernFieldName
                                        << ": "
                                        << fieldName);
        }
    }

    if (_clusterTime && _opTime) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Can not specify both " << kAfterClusterTimeFieldName
                                    << " and "
                                    << kAfterOpTimeFieldName);
    }

    // Note: 'available' should not be used with after cluster time, as cluster time can wait for
    // replication whereas the premise of 'available' is to avoid waiting.
    if (_clusterTime && getLevel() != ReadConcernLevel::kMajorityReadConcern &&
        getLevel() != ReadConcernLevel::kLocalReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAfterClusterTimeFieldName << " field can be set only if "
                                    << kLevelFieldName
                                    << " is equal to "
                                    << kMajorityReadConcernStr
                                    << " or "
                                    << kLocalReadConcernStr);
    }

    if (_clusterTime && _clusterTime == LogicalTime::kUninitialized) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAfterClusterTimeFieldName << " cannot be a null timestamp");
    }

    return Status::OK();
}

void ReadConcernArgs::appendInfo(BSONObjBuilder* builder) const {
    BSONObjBuilder rcBuilder(builder->subobjStart(kReadConcernFieldName));

    if (_level) {
        string levelName;
        switch (_level.get()) {
            case ReadConcernLevel::kLocalReadConcern:
                levelName = kLocalReadConcernStr;
                break;

            case ReadConcernLevel::kMajorityReadConcern:
                levelName = kMajorityReadConcernStr;
                break;

            case ReadConcernLevel::kLinearizableReadConcern:
                levelName = kLinearizableReadConcernStr;
                break;

            case ReadConcernLevel::kAvailableReadConcern:
                levelName = kAvailableReadConcernStr;
                break;

            default:
                fassert(28754, false);
        }

        rcBuilder.append(kLevelFieldName, levelName);
    }

    if (_opTime) {
        _opTime->append(&rcBuilder, kAfterOpTimeFieldName);
    }

    if (_clusterTime) {
        rcBuilder.append(kAfterClusterTimeFieldName, _clusterTime->asTimestamp());
    }

    rcBuilder.done();
}

}  // namespace repl
}  // namespace mongo
