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

#include "mongo/db/repl/read_concern_args.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

// The "kImplicitDefault" read concern, used by internal operations, is deliberately empty (no
// 'level' specified). This allows internal operations to specify a read concern, while still
// allowing it to be either local or available on sharded secondaries.
const ReadConcernArgs ReadConcernArgs::kImplicitDefault;
// The "kLocal" read concern just specifies that the read concern is local. This is used for
// internal operations intended to be run only on a certain target cluster member.
const ReadConcernArgs ReadConcernArgs::kLocal(ReadConcernLevel::kLocalReadConcern);
const ReadConcernArgs ReadConcernArgs::kMajority(ReadConcernLevel::kMajorityReadConcern);
const ReadConcernArgs ReadConcernArgs::kAvailable(ReadConcernLevel::kAvailableReadConcern);
const ReadConcernArgs ReadConcernArgs::kLinearizable(ReadConcernLevel::kLinearizableReadConcern);
const ReadConcernArgs ReadConcernArgs::kSnapshot(ReadConcernLevel::kSnapshotReadConcern);

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

Status ReadConcernArgs::parse(ReadConcernIdl inner) {
    if (auto& opTime = inner.getAfterOpTime()) {
        _opTime = *opTime;
    }
    _afterClusterTime = inner.getAfterClusterTime();
    _atClusterTime = inner.getAtClusterTime();
    if (auto level = inner.getLevel()) {
        _level = *level;
    }
    if (auto allowTxnTableSnapshot = inner.getAllowTransactionTableSnapshot()) {
        _allowTransactionTableSnapshot = *allowTxnTableSnapshot;
    }
    if (auto wait = inner.getWaitLastStableRecoveryTimestamp()) {
        _waitLastStableRecoveryTimestamp = *wait;
    }
    if (auto provenance = inner.getProvenance()) {
        _provenance = *provenance;
    }

    if (_afterClusterTime && _opTime) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream()
                          << "Can not specify both " << ReadConcernIdl::kAfterClusterTimeFieldName
                          << " and " << ReadConcernIdl::kAfterOpTimeFieldName);
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
                          << kLevelFieldName << " is equal to "
                          << readConcernLevels::toString(ReadConcernLevel::kMajorityReadConcern)
                          << ", "
                          << readConcernLevels::toString(ReadConcernLevel::kLocalReadConcern)
                          << ", or "
                          << readConcernLevels::toString(ReadConcernLevel::kSnapshotReadConcern));
    }

    if (_opTime && getLevel() == ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream()
                          << kAfterOpTimeFieldName << " field cannot be set if " << kLevelFieldName
                          << " is equal to "
                          << readConcernLevels::toString(ReadConcernLevel::kSnapshotReadConcern));
    }

    if (_atClusterTime && getLevel() != ReadConcernLevel::kSnapshotReadConcern) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream()
                          << kAtClusterTimeFieldName << " field can be set only if "
                          << kLevelFieldName << " is equal to "
                          << readConcernLevels::toString(ReadConcernLevel::kSnapshotReadConcern));
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

bool ReadConcernArgs::isImplicitDefault() const {
    return getProvenance().isImplicitDefault();
}

Status ReadConcernArgs::initialize(const BSONElement& readConcernElem) {
    invariant(isEmpty());  // only legal to call on uninitialized object.
    _specified = false;
    if (readConcernElem.eoo()) {
        return Status::OK();
    }

    dassert(readConcernElem.fieldNameStringData() == kReadConcernFieldName);

    if (readConcernElem.type() != BSONType::object) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << kReadConcernFieldName << " field should be an object");
    }

    return parse(readConcernElem.Obj());
}

Status ReadConcernArgs::parse(const BSONObj& readConcernObj) {
    invariant(isEmpty());  // only legal to call on uninitialized object.

    try {
        auto inner = ReadConcernIdl::parse(readConcernObj, IDLParserContext("readConcern"));
        return parse(std::move(inner));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

ReadConcernArgs ReadConcernArgs::fromBSONThrows(const BSONObj& readConcernObj) {
    ReadConcernArgs rc;
    uassertStatusOK(rc.parse(readConcernObj));
    return rc;
}

ReadConcernArgs ReadConcernArgs::fromIDLThrows(ReadConcernIdl readConcern) {
    ReadConcernArgs rc;
    uassertStatusOK(rc.parse(std::move(readConcern)));
    return rc;
}

ReadConcernArgs::MajorityReadMechanism ReadConcernArgs::getMajorityReadMechanism() const {
    invariant(*_level == ReadConcernLevel::kMajorityReadConcern);
    return _majorityReadMechanism;
}

ReadConcernIdl ReadConcernArgs::toReadConcernIdl() const {
    ReadConcernIdl rc;
    rc.setLevel(_level);
    rc.setAfterOpTime(_opTime);
    rc.setAfterClusterTime(_afterClusterTime);
    rc.setAtClusterTime(_atClusterTime);
    if (_allowTransactionTableSnapshot) {
        rc.setAllowTransactionTableSnapshot(_allowTransactionTableSnapshot);
    }
    if (_waitLastStableRecoveryTimestamp) {
        rc.setWaitLastStableRecoveryTimestamp(_waitLastStableRecoveryTimestamp);
    }
    rc.setProvenance(_provenance.getSource());

    return rc;
}

void ReadConcernArgs::_appendInfoInner(BSONObjBuilder* builder) const {
    toReadConcernIdl().serialize(builder);
}

void ReadConcernArgs::appendInfo(BSONObjBuilder* builder) const {
    BSONObjBuilder rcBuilder(builder->subobjStart(kReadConcernFieldName));
    _appendInfoInner(&rcBuilder);
    rcBuilder.done();
}

StringData readConcernLevels::toString(ReadConcernLevel level) {
    return ReadConcernLevel_serializer(level);
}

}  // namespace repl
}  // namespace mongo
