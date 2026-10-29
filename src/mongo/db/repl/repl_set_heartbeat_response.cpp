// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/repl_set_heartbeat_response.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
namespace {

const std::string kConfigFieldName = "config";
const std::string kConfigVersionFieldName = "v";
const std::string kConfigTermFieldName = "configTerm";
const std::string kElectionTimeFieldName = "electionTime";
const std::string kMemberStateFieldName = "state";
const std::string kOkFieldName = "ok";
const std::string kAppliedOpTimeFieldName = "opTime";
const std::string kAppliedWallTimeFieldName = "wallTime";
const std::string kWrittenOpTimeFieldName = "writtenOpTime";
const std::string kWrittenWallTimeFieldName = "writtenWallTime";
const std::string kDurableOpTimeFieldName = "durableOpTime";
const std::string kDurableWallTimeFieldName = "durableWallTime";
const std::string kPrimaryIdFieldName = "primaryId";
const std::string kReplSetFieldName = "set";
const std::string kSyncSourceFieldName = "syncingTo";
const std::string kTermFieldName = "term";
const std::string kTimestampFieldName = "ts";
const std::string kIsElectableFieldName = "electable";
const std::string kLastStableRecoveryTimestampFieldName = "lastStableRecoveryTimestamp";

}  // namespace

void ReplSetHeartbeatResponse::addToBSON(BSONObjBuilder* builder) const {
    builder->append(kOkFieldName, 1.0);
    if (_electionTimeSet) {
        // TODO: SERVER-108961
        // We currently set the electionTime using Timestamp.toLL() which forms the 64b integer
        // value with the seconds-since-unix-epoch value in the high 32b and the increment in the
        // low 32b. this is notably *NOT* the number of milliseconds since the unix epoch, and is
        // only correct on accident, since during `initialize` we reverse this same incorrect
        // conversion. The only time this value is *wrong* is when stored in the BSON document as a
        // Date - it is not an equivalent value when interpreted as msec since the epoch. However,
        // we believe it is correct on both ends before/after performing the conversions.
        builder->appendDate(kElectionTimeFieldName,
                            Date_t::fromMillisSinceEpoch(_electionTime.asLL()));
    }
    if (_configSet) {
        *builder << kConfigFieldName << _config.toBSON();
    }
    if (_stateSet) {
        builder->appendNumber(kMemberStateFieldName, _state.s);
    }
    if (_configVersion != -1) {
        *builder << kConfigVersionFieldName << _configVersion;
        *builder << kConfigTermFieldName << _configTerm;
    }
    if (!_setName.empty()) {
        *builder << kReplSetFieldName << _setName;
    }
    if (!_syncingTo.empty()) {
        *builder << kSyncSourceFieldName << _syncingTo.toString();
    }
    if (_term != -1) {
        builder->append(kTermFieldName, _term);
    }
    if (_primaryIdSet) {
        builder->append(kPrimaryIdFieldName, _primaryId);
    }
    if (_appliedOpTimeSet) {
        _appliedOpTime.append(kAppliedOpTimeFieldName, builder);
        builder->appendDate(kAppliedWallTimeFieldName, _appliedWallTime);
    }
    if (_writtenOpTimeSet) {
        _writtenOpTime.append(kWrittenOpTimeFieldName, builder);
        builder->appendDate(kWrittenWallTimeFieldName, _writtenWallTime);
    }
    if (_durableOpTimeSet) {
        _durableOpTime.append(kDurableOpTimeFieldName, builder);
        builder->appendDate(kDurableWallTimeFieldName, _durableWallTime);
    }
    if (_electableSet) {
        *builder << kIsElectableFieldName << _electable;
    }
    if (_lastStableRecoveryTimestamp) {
        builder->append(kLastStableRecoveryTimestampFieldName, *_lastStableRecoveryTimestamp);
    }
}

BSONObj ReplSetHeartbeatResponse::toBSON() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.obj();
}

Status ReplSetHeartbeatResponse::initialize(const BSONObj& doc, long long term) {
    auto status = getStatusFromCommandResult(doc);
    if (!status.isOK()) {
        return status;
    }

    const BSONElement replSetNameElement = doc[kReplSetFieldName];
    if (replSetNameElement.eoo()) {
        _setName.clear();
    } else if (replSetNameElement.type() != BSONType::string) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kReplSetFieldName
                                    << "\" field in response to replSetHeartbeat to have "
                                       "type String, but found "
                                    << typeName(replSetNameElement.type()));
    } else {
        _setName = replSetNameElement.String();
    }

    const BSONElement electionTimeElement = doc[kElectionTimeFieldName];
    if (electionTimeElement.eoo()) {
        _electionTimeSet = false;
    } else if (electionTimeElement.type() == BSONType::date) {
        _electionTimeSet = true;
        // TODO: SERVER-108961
        // This explicit conversion from date to Timestamp is not correct in general, and a longer
        // explanation of why is in the `addToBSON` method. This needs to be changed to only
        // serialize timestamps into BSON, not the `Date_t` type.
        _electionTime = Timestamp(electionTimeElement.date());
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kElectionTimeFieldName
                                    << "\" field in response to replSetHeartbeat "
                                       "command to have type Date, but found type "
                                    << typeName(electionTimeElement.type()));
    }

    Status termStatus = bsonExtractIntegerField(doc, kTermFieldName, &_term);
    if (!termStatus.isOK() && termStatus != ErrorCodes::NoSuchKey) {
        return termStatus;
    }

    status = bsonExtractOpTimeField(doc, kDurableOpTimeFieldName, &_durableOpTime);
    if (!status.isOK()) {
        return status;
    }

    BSONElement durableWallTimeElement;
    _durableWallTime = Date_t();
    status = bsonExtractTypedField(
        doc, kDurableWallTimeFieldName, BSONType::date, &durableWallTimeElement);
    if (!status.isOK()) {
        return status;
    }
    _durableWallTime = durableWallTimeElement.Date();
    _durableOpTimeSet = true;

    status = bsonExtractBooleanField(doc, kIsElectableFieldName, &_electable);
    if (!status.isOK()) {
        _electableSet = false;
    } else {
        _electableSet = true;
    }

    // In V1, heartbeats OpTime is type Object and we construct an OpTime out of its nested fields.
    status = bsonExtractOpTimeField(doc, kAppliedOpTimeFieldName, &_appliedOpTime);
    if (!status.isOK()) {
        return status;
    }

    BSONElement appliedWallTimeElement;
    _appliedWallTime = Date_t();
    status = bsonExtractTypedField(
        doc, kAppliedWallTimeFieldName, BSONType::date, &appliedWallTimeElement);
    if (!status.isOK()) {
        return status;
    }
    _appliedWallTime = appliedWallTimeElement.Date();
    _appliedOpTimeSet = true;

    status = bsonExtractOpTimeField(doc, kWrittenOpTimeFieldName, &_writtenOpTime);
    if (!status.isOK()) {
        if (status.code() == ErrorCodes::NoSuchKey) {
            _writtenOpTime = _appliedOpTime;
        } else {
            return status;
        }
    }

    BSONElement writtenWallTimeElement;
    _writtenWallTime = Date_t();
    status = bsonExtractTypedField(
        doc, kWrittenWallTimeFieldName, BSONType::date, &writtenWallTimeElement);
    if (!status.isOK()) {
        if (status.code() == ErrorCodes::NoSuchKey) {
            _writtenWallTime = _appliedWallTime;
        } else {
            return status;
        }
    } else {
        _writtenWallTime = writtenWallTimeElement.Date();
    }
    _writtenOpTimeSet = true;

    const BSONElement memberStateElement = doc[kMemberStateFieldName];
    if (memberStateElement.eoo()) {
        _stateSet = false;
    } else if (memberStateElement.type() != BSONType::numberInt &&
               memberStateElement.type() != BSONType::numberLong) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "Expected \"" << kMemberStateFieldName
                          << "\" field in response to replSetHeartbeat "
                             "command to have type NumberInt or NumberLong, but found type "
                          << typeName(memberStateElement.type()));
    } else {
        long long stateInt = memberStateElement.numberLong();
        if (stateInt < 0 || stateInt > MemberState::RS_MAX) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "Value for \"" << kMemberStateFieldName
                              << "\" in response to replSetHeartbeat is "
                                 "out of range; legal values are non-negative and no more than "
                              << MemberState::RS_MAX);
        }
        _stateSet = true;
        _state = MemberState(static_cast<int>(stateInt));
    }

    const BSONElement configVersionElement = doc[kConfigVersionFieldName];
    if (configVersionElement.eoo()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "Response to replSetHeartbeat missing required \""
                                    << kConfigVersionFieldName << "\" field");
    }
    if (configVersionElement.type() != BSONType::numberInt &&
        configVersionElement.type() != BSONType::numberLong) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kConfigVersionFieldName
                                    << "\" field in response to replSetHeartbeat to have "
                                       "type NumberInt/NumberLong, but found "
                                    << typeName(configVersionElement.type()));
    }
    _configVersion = configVersionElement.numberLong();

    // Allow a missing term field for backward compatibility.
    const BSONElement configTermElement = doc[kConfigTermFieldName];
    if (!configTermElement.eoo() &&
        (configTermElement.type() == BSONType::numberInt ||
         configTermElement.type() == BSONType::numberLong)) {
        _configTerm = configTermElement.numberLong();
    }

    const BSONElement syncingToElement = doc[kSyncSourceFieldName];
    if (syncingToElement.eoo()) {
        _syncingTo = HostAndPort();
    } else if (syncingToElement.type() != BSONType::string) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kSyncSourceFieldName
                                    << "\" field in response to replSetHeartbeat to "
                                       "have type String, but found "
                                    << typeName(syncingToElement.type()));
    } else {
        _syncingTo = HostAndPort(syncingToElement.String());
    }

    const BSONElement lastStableRecoveryTimestampElement =
        doc[kLastStableRecoveryTimestampFieldName];
    if (!lastStableRecoveryTimestampElement.eoo()) {
        if (lastStableRecoveryTimestampElement.type() != BSONType::timestamp) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream()
                              << "Expected \"" << kLastStableRecoveryTimestampFieldName
                              << "\" field in response to replSetHeartbeat to have type Timestamp, "
                                 "but found "
                              << typeName(lastStableRecoveryTimestampElement.type()));
        }
        _lastStableRecoveryTimestamp = lastStableRecoveryTimestampElement.timestamp();
    }

    const BSONElement rsConfigElement = doc[kConfigFieldName];
    if (rsConfigElement.eoo()) {
        _configSet = false;
        _config = ReplSetConfig();
        return Status::OK();
    } else if (rsConfigElement.type() != BSONType::object) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kConfigFieldName
                                    << "\" in response to replSetHeartbeat to have type "
                                       "Object, but found "
                                    << typeName(rsConfigElement.type()));
    }
    _configSet = true;

    try {
        _config = ReplSetConfig::parse(rsConfigElement.Obj());
    } catch (const DBException& e) {
        return e.toStatus();
    }
    return Status::OK();
}

MemberState ReplSetHeartbeatResponse::getState() const {
    invariant(_stateSet);
    return _state;
}

Timestamp ReplSetHeartbeatResponse::getElectionTime() const {
    invariant(_electionTimeSet);
    return _electionTime;
}

const ReplSetConfig& ReplSetHeartbeatResponse::getConfig() const {
    invariant(_configSet);
    return _config;
}

long long ReplSetHeartbeatResponse::getPrimaryId() const {
    invariant(_primaryIdSet);
    return _primaryId;
}

OpTime ReplSetHeartbeatResponse::getAppliedOpTime() const {
    invariant(_appliedOpTimeSet);
    return _appliedOpTime;
}

OpTimeAndWallTime ReplSetHeartbeatResponse::getAppliedOpTimeAndWallTime() const {
    invariant(_appliedOpTimeSet);
    return {_appliedOpTime, _appliedWallTime};
}

OpTime ReplSetHeartbeatResponse::getWrittenOpTime() const {
    invariant(_writtenOpTimeSet);
    return _writtenOpTime;
}

OpTimeAndWallTime ReplSetHeartbeatResponse::getWrittenOpTimeAndWallTime() const {
    invariant(_writtenOpTimeSet);
    return {_writtenOpTime, _writtenWallTime};
}

OpTime ReplSetHeartbeatResponse::getDurableOpTime() const {
    invariant(_durableOpTimeSet);
    return _durableOpTime;
}

OpTimeAndWallTime ReplSetHeartbeatResponse::getDurableOpTimeAndWallTime() const {
    invariant(_durableOpTimeSet);
    return {_durableOpTime, _durableWallTime};
}

bool ReplSetHeartbeatResponse::isElectable() const {
    invariant(_electableSet);
    return _electable;
}

}  // namespace repl
}  // namespace mongo
