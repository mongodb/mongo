
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/read_concern_args.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/util/mongoutils/str.h"

using std::string;

namespace mongo {
namespace repl {

namespace {

constexpr StringData kLocalReadConcernStr = "local"_sd;
constexpr StringData kMajorityReadConcernStr = "majority"_sd;
constexpr StringData kLinearizableReadConcernStr = "linearizable"_sd;
constexpr StringData kAvailableReadConcernStr = "available"_sd;
constexpr StringData kSnapshotReadConcernStr = "snapshot"_sd;

}  // unnamed namespace

const string ReadConcernArgs::kReadConcernFieldName("readConcern");
const string ReadConcernArgs::kAfterOpTimeFieldName("afterOpTime");
const string ReadConcernArgs::kAfterClusterTimeFieldName("afterClusterTime");
const string ReadConcernArgs::kAtClusterTimeFieldName("atClusterTime");

const string ReadConcernArgs::kLevelFieldName("level");

const OperationContext::Decoration<ReadConcernArgs> handle =
    OperationContext::declareDecoration<ReadConcernArgs>();

ReadConcernArgs& ReadConcernArgs::get(OperationContext* opCtx) {
    return handle(opCtx);
}

const ReadConcernArgs& ReadConcernArgs::get(const OperationContext* opCtx) {
    return handle(opCtx);
}


ReadConcernArgs::ReadConcernArgs() = default;

ReadConcernArgs::ReadConcernArgs(boost::optional<ReadConcernLevel> level)
    : _level(std::move(level)) {}

ReadConcernArgs::ReadConcernArgs(boost::optional<OpTime> opTime,
                                 boost::optional<ReadConcernLevel> level)
    : _opTime(std::move(opTime)), _level(std::move(level)) {}

ReadConcernArgs::ReadConcernArgs(boost::optional<LogicalTime> clusterTime,
                                 boost::optional<ReadConcernLevel> level)
    : _afterClusterTime(std::move(clusterTime)), _level(std::move(level)) {}

std::string ReadConcernArgs::toString() const {
    return toBSON().toString();
}

BSONObj ReadConcernArgs::toBSON() const {
    BSONObjBuilder bob;
    appendInfo(&bob);
    return bob.obj();
}

bool ReadConcernArgs::isEmpty() const {
    return !_afterClusterTime && !_opTime && !_atClusterTime && !_level;
}

ReadConcernLevel ReadConcernArgs::getLevel() const {
    return _level.value_or(ReadConcernLevel::kLocalReadConcern);
}

ReadConcernLevel ReadConcernArgs::getOriginalLevel() const {
    return _originalLevel.value_or(getLevel());
}

bool ReadConcernArgs::hasLevel() const {
    return _level.is_initialized();
}

boost::optional<OpTime> ReadConcernArgs::getArgsOpTime() const {
    return _opTime;
}

boost::optional<LogicalTime> ReadConcernArgs::getArgsAfterClusterTime() const {
    return _afterClusterTime;
}

boost::optional<LogicalTime> ReadConcernArgs::getArgsAtClusterTime() const {
    return _atClusterTime;
}

Status ReadConcernArgs::initialize(const BSONElement& readConcernElem) {
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
            Timestamp afterClusterTime;
            auto afterClusterTimeStatus = bsonExtractTimestampField(
                readConcernObj, kAfterClusterTimeFieldName, &afterClusterTime);
            if (!afterClusterTimeStatus.isOK()) {
                return afterClusterTimeStatus;
            }
            _afterClusterTime = LogicalTime(afterClusterTime);
        } else if (fieldName == kAtClusterTimeFieldName) {
            Timestamp atClusterTime;
            auto atClusterTimeStatus =
                bsonExtractTimestampField(readConcernObj, kAtClusterTimeFieldName, &atClusterTime);
            if (!atClusterTimeStatus.isOK()) {
                return atClusterTimeStatus;
            }
            _atClusterTime = LogicalTime(atClusterTime);
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
            } else if (levelString == kSnapshotReadConcernStr) {
                _level = ReadConcernLevel::kSnapshotReadConcern;
            } else {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << kReadConcernFieldName << '.' << kLevelFieldName
                                            << " must be either 'local', 'majority', "
                                               "'linearizable', 'available', or 'snapshot'");
            }
        } else {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "Unrecognized option in " << kReadConcernFieldName
                                        << ": "
                                        << fieldName);
        }
    }

    if (_afterClusterTime && _opTime) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Can not specify both " << kAfterClusterTimeFieldName
                                    << " and "
                                    << kAfterOpTimeFieldName);
    }

    if (_afterClusterTime && _atClusterTime) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Can not specify both " << kAfterClusterTimeFieldName
                                    << " and "
                                    << kAtClusterTimeFieldName);
    }

    // Note: 'available' should not be used with after cluster time, as cluster time can wait for
    // replication whereas the premise of 'available' is to avoid waiting. 'linearizable' should not
    // be used with after cluster time, since linearizable reads are inherently causally consistent.
    if (_afterClusterTime && getLevel() != ReadConcernLevel::kMajorityReadConcern &&
        getLevel() != ReadConcernLevel::kLocalReadConcern &&
        getLevel() != ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAfterClusterTimeFieldName << " field can be set only if "
                                    << kLevelFieldName
                                    << " is equal to "
                                    << kMajorityReadConcernStr
                                    << ", "
                                    << kLocalReadConcernStr
                                    << ", or "
                                    << kSnapshotReadConcernStr);
    }

    if (_opTime && getLevel() == ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAfterOpTimeFieldName << " field cannot be set if "
                                    << kLevelFieldName
                                    << " is equal to "
                                    << kSnapshotReadConcernStr);
    }

    if (_atClusterTime && getLevel() != ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAtClusterTimeFieldName << " field can be set only if "
                                    << kLevelFieldName
                                    << " is equal to "
                                    << kSnapshotReadConcernStr);
    }

    if (_afterClusterTime && _afterClusterTime == LogicalTime::kUninitialized) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAfterClusterTimeFieldName << " cannot be a null timestamp");
    }

    if (_atClusterTime && _atClusterTime == LogicalTime::kUninitialized) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAtClusterTimeFieldName << " cannot be a null timestamp");
    }

    return Status::OK();
}

void ReadConcernArgs::setMajorityReadMechanism(MajorityReadMechanism mechanism) {
    invariant(*_level == ReadConcernLevel::kMajorityReadConcern);
    _majorityReadMechanism = mechanism;
}

ReadConcernArgs::MajorityReadMechanism ReadConcernArgs::getMajorityReadMechanism() const {
    invariant(*_level == ReadConcernLevel::kMajorityReadConcern);
    return _majorityReadMechanism;
}

Status ReadConcernArgs::upconvertReadConcernLevelToSnapshot() {
    if (_level && *_level != ReadConcernLevel::kSnapshotReadConcern &&
        *_level != ReadConcernLevel::kMajorityReadConcern &&
        *_level != ReadConcernLevel::kLocalReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      "The readConcern level must be either 'local' or 'majority' in order to "
                      "upconvert the readConcern level to 'snapshot'");
    }

    if (_opTime) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Cannot upconvert the readConcern level to 'snapshot' when '"
                                    << kAfterOpTimeFieldName
                                    << "' is provided");
    }

    _originalLevel = _level ? *_level : ReadConcernLevel::kLocalReadConcern;
    _level = ReadConcernLevel::kSnapshotReadConcern;
    return Status::OK();
}

void ReadConcernArgs::appendInfo(BSONObjBuilder* builder) const {
    BSONObjBuilder rcBuilder(builder->subobjStart(kReadConcernFieldName));

    if (_level) {
        StringData levelName;
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

            case ReadConcernLevel::kSnapshotReadConcern:
                levelName = kSnapshotReadConcernStr;
                break;

            default:
                MONGO_UNREACHABLE;
        }

        rcBuilder.append(kLevelFieldName, levelName);
    }

    if (_opTime) {
        _opTime->append(&rcBuilder, kAfterOpTimeFieldName);
    }

    if (_afterClusterTime) {
        rcBuilder.append(kAfterClusterTimeFieldName, _afterClusterTime->asTimestamp());
    }

    if (_atClusterTime) {
        rcBuilder.append(kAtClusterTimeFieldName, _atClusterTime->asTimestamp());
    }

    rcBuilder.done();
}

}  // namespace repl
}  // namespace mongo
