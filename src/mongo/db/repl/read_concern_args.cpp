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


#include "mongo/platform/basic.h"

#include "mongo/db/repl/read_concern_args.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


using std::string;

namespace mongo {
namespace repl {

const OperationContext::Decoration<ReadConcernArgs> handle =
    OperationContext::declareDecoration<ReadConcernArgs>();

ReadConcernArgs& ReadConcernArgs::get(OperationContext* opCtx) {
    return handle(opCtx);
}

const ReadConcernArgs& ReadConcernArgs::get(const OperationContext* opCtx) {
    return handle(opCtx);
}


// The "kImplicitDefault" read concern, used by internal operations, is deliberately empty (no
// 'level' specified). This allows internal operations to specify a read concern, while still
// allowing it to be either local or available on sharded secondaries.
const BSONObj ReadConcernArgs::kImplicitDefault;

// The "kLocal" read concern just specifies that the read concern is local. This is used for
// internal operations intended to be run only on a certain target cluster member.
const BSONObj ReadConcernArgs::kLocal = BSON(kLevelFieldName << readConcernLevels::kLocalName);

ReadConcernArgs::ReadConcernArgs() : _specified(false) {}

ReadConcernArgs::ReadConcernArgs(boost::optional<ReadConcernLevel> level)
    : _level(std::move(level)), _specified(_level) {}

ReadConcernArgs::ReadConcernArgs(boost::optional<OpTime> opTime,
                                 boost::optional<ReadConcernLevel> level)
    : _opTime(std::move(opTime)), _level(std::move(level)), _specified(_opTime || _level) {}

ReadConcernArgs::ReadConcernArgs(boost::optional<LogicalTime> afterClusterTime,
                                 boost::optional<ReadConcernLevel> level)
    : _afterClusterTime(std::move(afterClusterTime)),
      _level(std::move(level)),
      _specified(_afterClusterTime || _level) {}

std::string ReadConcernArgs::toString() const {
    return toBSON().toString();
}

BSONObj ReadConcernArgs::toBSON() const {
    BSONObjBuilder bob;
    appendInfo(&bob);
    return bob.obj();
}

BSONObj ReadConcernArgs::toBSONInner() const {
    BSONObjBuilder bob;
    _appendInfoInner(&bob);
    return bob.obj();
}

bool ReadConcernArgs::isEmpty() const {
    return !_afterClusterTime && !_opTime && !_atClusterTime && !_level;
}

bool ReadConcernArgs::isSpecified() const {
    return _specified;
}

bool ReadConcernArgs::isImplicitDefault() const {
    return getProvenance().isImplicitDefault();
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

boost::optional<LogicalTime> ReadConcernArgs::getArgsAfterClusterTime() const {
    return _afterClusterTime;
}

boost::optional<LogicalTime> ReadConcernArgs::getArgsAtClusterTime() const {
    return _atClusterTime;
}

Status ReadConcernArgs::initialize(const BSONElement& readConcernElem) {
    invariant(isEmpty());  // only legal to call on uninitialized object.
    _specified = false;
    if (readConcernElem.eoo()) {
        return Status::OK();
    }

    dassert(readConcernElem.fieldNameStringData() == kReadConcernFieldName);

    if (readConcernElem.type() != Object) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << kReadConcernFieldName << " field should be an object");
    }

    return parse(readConcernElem.Obj());
}

Status ReadConcernArgs::parse(const BSONObj& readConcernObj) {
    invariant(isEmpty());  // only legal to call on uninitialized object.
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

            _level = readConcernLevels::fromString(levelString);
            if (!_level) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << kReadConcernFieldName << '.' << kLevelFieldName
                                            << " must be either '" << readConcernLevels::kLocalName
                                            << "', '" << readConcernLevels::kMajorityName << "', '"
                                            << readConcernLevels::kLinearizableName << "', '"
                                            << readConcernLevels::kAvailableName << "', or '"
                                            << readConcernLevels::kSnapshotName << "'");
            }
        } else if (fieldName == kAllowTransactionTableSnapshot) {
            auto status = bsonExtractBooleanField(
                readConcernObj, kAllowTransactionTableSnapshot, &_allowTransactionTableSnapshot);
            if (!status.isOK()) {
                return status;
            }
        } else if (fieldName == ReadWriteConcernProvenance::kSourceFieldName) {
            try {
                _provenance = ReadWriteConcernProvenance::parse(
                    IDLParserContext("ReadConcernArgs::parse"), readConcernObj);
            } catch (const DBException&) {
                return exceptionToStatus();
            }
        } else {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "Unrecognized option in " << kReadConcernFieldName
                                        << ": " << fieldName);
        }
    }

    if (_afterClusterTime && _opTime) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Can not specify both " << kAfterClusterTimeFieldName
                                    << " and " << kAfterOpTimeFieldName);
    }

    if (_afterClusterTime && _atClusterTime) {
        return Status(ErrorCodes::InvalidOptions,
                      "Specifying a timestamp for readConcern snapshot in a causally consistent "
                      "session is not allowed. See "
                      "https://docs.mongodb.com/manual/core/read-isolation-consistency-recency/"
                      "#causal-consistency");
    }

    // Note: 'available' should not be used with after cluster time, as cluster time can wait for
    // replication whereas the premise of 'available' is to avoid waiting. 'linearizable' should not
    // be used with after cluster time, since linearizable reads are inherently causally consistent.
    if (_afterClusterTime && getLevel() != ReadConcernLevel::kMajorityReadConcern &&
        getLevel() != ReadConcernLevel::kLocalReadConcern &&
        getLevel() != ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream()
                          << kAfterClusterTimeFieldName << " field can be set only if "
                          << kLevelFieldName << " is equal to " << readConcernLevels::kMajorityName
                          << ", " << readConcernLevels::kLocalName << ", or "
                          << readConcernLevels::kSnapshotName);
    }

    if (_opTime && getLevel() == ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream()
                          << kAfterOpTimeFieldName << " field cannot be set if " << kLevelFieldName
                          << " is equal to " << readConcernLevels::kSnapshotName);
    }

    if (_atClusterTime && getLevel() != ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAtClusterTimeFieldName << " field can be set only if "
                                    << kLevelFieldName << " is equal to "
                                    << readConcernLevels::kSnapshotName);
    }

    // Make sure that atClusterTime wasn't specified with zero seconds.
    if (_atClusterTime && _atClusterTime->asTimestamp().isNull()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAtClusterTimeFieldName << " cannot be a null timestamp");
    }

    // It's okay for afterClusterTime to be specified with zero seconds, but not an uninitialized
    // timestamp.
    if (_afterClusterTime && _afterClusterTime == LogicalTime::kUninitialized) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << kAfterClusterTimeFieldName << " cannot be a null timestamp");
    }

    _specified = true;
    return Status::OK();
}

ReadConcernArgs ReadConcernArgs::fromBSONThrows(const BSONObj& readConcernObj) {
    ReadConcernArgs rc;
    uassertStatusOK(rc.parse(readConcernObj));
    return rc;
}

void ReadConcernArgs::setMajorityReadMechanism(MajorityReadMechanism mechanism) {
    invariant(*_level == ReadConcernLevel::kMajorityReadConcern);
    _majorityReadMechanism = mechanism;
}

ReadConcernArgs::MajorityReadMechanism ReadConcernArgs::getMajorityReadMechanism() const {
    invariant(*_level == ReadConcernLevel::kMajorityReadConcern);
    return _majorityReadMechanism;
}

bool ReadConcernArgs::isSpeculativeMajority() const {
    return _level && *_level == ReadConcernLevel::kMajorityReadConcern &&
        _majorityReadMechanism == MajorityReadMechanism::kSpeculative;
}

void ReadConcernArgs::_appendInfoInner(BSONObjBuilder* builder) const {
    if (_level) {
        builder->append(kLevelFieldName, readConcernLevels::toString(_level.get()));
    }

    if (_opTime) {
        _opTime->append(builder, kAfterOpTimeFieldName.toString());
    }

    if (_afterClusterTime) {
        builder->append(kAfterClusterTimeFieldName, _afterClusterTime->asTimestamp());
    }

    if (_atClusterTime) {
        builder->append(kAtClusterTimeFieldName, _atClusterTime->asTimestamp());
    }

    if (_allowTransactionTableSnapshot) {
        builder->append(kAllowTransactionTableSnapshot, _allowTransactionTableSnapshot);
    }

    _provenance.serialize(builder);
}

void ReadConcernArgs::appendInfo(BSONObjBuilder* builder) const {
    BSONObjBuilder rcBuilder(builder->subobjStart(kReadConcernFieldName));
    _appendInfoInner(&rcBuilder);
    rcBuilder.done();
}

boost::optional<ReadConcernLevel> readConcernLevels::fromString(StringData levelString) {
    if (levelString == readConcernLevels::kLocalName) {
        return ReadConcernLevel::kLocalReadConcern;
    } else if (levelString == readConcernLevels::kMajorityName) {
        return ReadConcernLevel::kMajorityReadConcern;
    } else if (levelString == readConcernLevels::kLinearizableName) {
        return ReadConcernLevel::kLinearizableReadConcern;
    } else if (levelString == readConcernLevels::kAvailableName) {
        return ReadConcernLevel::kAvailableReadConcern;
    } else if (levelString == readConcernLevels::kSnapshotName) {
        return ReadConcernLevel::kSnapshotReadConcern;
    } else {
        return boost::none;
    }
}

StringData readConcernLevels::toString(ReadConcernLevel level) {
    switch (level) {
        case ReadConcernLevel::kLocalReadConcern:
            return kLocalName;

        case ReadConcernLevel::kMajorityReadConcern:
            return kMajorityName;

        case ReadConcernLevel::kLinearizableReadConcern:
            return kLinearizableName;

        case ReadConcernLevel::kAvailableReadConcern:
            return kAvailableName;

        case ReadConcernLevel::kSnapshotReadConcern:
            return kSnapshotName;

        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace repl
}  // namespace mongo
