/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/repl_set_heartbeat_response.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

const std::string kConfigFieldName = "config";
const std::string kConfigVersionFieldName = "v";
const std::string kElectionTimeFieldName = "electionTime";
const std::string kErrMsgFieldName = "errmsg";
const std::string kErrorCodeFieldName = "code";
const std::string kHasDataFieldName = "hasData";
const std::string kHasStateDisagreementFieldName = "stateDisagreement";
const std::string kHbMessageFieldName = "hbmsg";
const std::string kIsElectableFieldName = "e";
const std::string kIsReplSetFieldName = "rs";
const std::string kMemberStateFieldName = "state";
const std::string kMismatchFieldName = "mismatch";
const std::string kOkFieldName = "ok";
const std::string kDurableOpTimeFieldName = "durableOpTime";
const std::string kAppliedOpTimeFieldName = "opTime";
const std::string kPrimaryIdFieldName = "primaryId";
const std::string kReplSetFieldName = "set";
const std::string kSyncSourceFieldName = "syncingTo";
const std::string kTermFieldName = "term";
const std::string kTimeFieldName = "time";
const std::string kTimestampFieldName = "ts";

}  // namespace

void ReplSetHeartbeatResponse::addToBSON(BSONObjBuilder* builder, bool isProtocolVersionV1) const {
    if (_mismatch) {
        *builder << kOkFieldName << 0.0;
        *builder << kMismatchFieldName << _mismatch;
        return;
    }

    builder->append(kOkFieldName, 1.0);
    if (_timeSet) {
        *builder << kTimeFieldName << durationCount<Seconds>(_time);
    }
    if (_electionTimeSet) {
        builder->appendDate(kElectionTimeFieldName,
                            Date_t::fromMillisSinceEpoch(_electionTime.asLL()));
    }
    if (_configSet) {
        *builder << kConfigFieldName << _config.toBSON();
    }
    if (_electableSet) {
        *builder << kIsElectableFieldName << _electable;
    }
    if (_isReplSet) {
        *builder << "rs" << _isReplSet;
    }
    if (_stateDisagreement) {
        *builder << kHasStateDisagreementFieldName << _stateDisagreement;
    }
    if (_stateSet) {
        builder->appendIntOrLL(kMemberStateFieldName, _state.s);
    }
    if (_configVersion != -1) {
        *builder << kConfigVersionFieldName << _configVersion;
    }
    *builder << kHbMessageFieldName << _hbmsg;
    if (!_setName.empty()) {
        *builder << kReplSetFieldName << _setName;
    }
    if (!_syncingTo.empty()) {
        *builder << kSyncSourceFieldName << _syncingTo.toString();
    }
    if (_hasDataSet) {
        builder->append(kHasDataFieldName, _hasData);
    }
    if (_term != -1) {
        builder->append(kTermFieldName, _term);
    }
    if (_primaryIdSet) {
        builder->append(kPrimaryIdFieldName, _primaryId);
    }
    if (_durableOpTimeSet) {
        _durableOpTime.append(builder, kDurableOpTimeFieldName);
    }
    if (_appliedOpTimeSet) {
        if (isProtocolVersionV1) {
            _appliedOpTime.append(builder, kAppliedOpTimeFieldName);
        } else {
            builder->appendDate(kAppliedOpTimeFieldName,
                                Date_t::fromMillisSinceEpoch(_appliedOpTime.getTimestamp().asLL()));
        }
    }
}

BSONObj ReplSetHeartbeatResponse::toBSON(bool isProtocolVersionV1) const {
    BSONObjBuilder builder;
    addToBSON(&builder, isProtocolVersionV1);
    return builder.obj();
}

Status ReplSetHeartbeatResponse::initialize(const BSONObj& doc, long long term) {
    // Old versions set this even though they returned not "ok"
    _mismatch = doc[kMismatchFieldName].trueValue();
    if (_mismatch)
        return Status(ErrorCodes::InconsistentReplicaSetNames, "replica set name doesn't match.");

    // Old versions sometimes set the replica set name ("set") but ok:0
    const BSONElement replSetNameElement = doc[kReplSetFieldName];
    if (replSetNameElement.eoo()) {
        _setName.clear();
    } else if (replSetNameElement.type() != String) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kReplSetFieldName
                                    << "\" field in response to replSetHeartbeat to have "
                                       "type String, but found "
                                    << typeName(replSetNameElement.type()));
    } else {
        _setName = replSetNameElement.String();
    }

    if (_setName.empty() && !doc[kOkFieldName].trueValue()) {
        std::string errMsg = doc[kErrMsgFieldName].str();

        BSONElement errCodeElem = doc[kErrorCodeFieldName];
        if (errCodeElem.ok()) {
            if (!errCodeElem.isNumber())
                return Status(ErrorCodes::BadValue, "Error code is not a number!");

            int errorCode = errCodeElem.numberInt();
            return Status(ErrorCodes::Error(errorCode), errMsg);
        }
        return Status(ErrorCodes::UnknownError, errMsg);
    }

    const BSONElement hasDataElement = doc[kHasDataFieldName];
    _hasDataSet = !hasDataElement.eoo();
    _hasData = hasDataElement.trueValue();

    const BSONElement electionTimeElement = doc[kElectionTimeFieldName];
    if (electionTimeElement.eoo()) {
        _electionTimeSet = false;
    } else if (electionTimeElement.type() == bsonTimestamp) {
        _electionTimeSet = true;
        _electionTime = electionTimeElement.timestamp();
    } else if (electionTimeElement.type() == Date) {
        _electionTimeSet = true;
        _electionTime = Timestamp(electionTimeElement.date());
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kElectionTimeFieldName
                                    << "\" field in response to replSetHeartbeat "
                                       "command to have type Date or Timestamp, but found type "
                                    << typeName(electionTimeElement.type()));
    }

    const BSONElement timeElement = doc[kTimeFieldName];
    if (timeElement.eoo()) {
        _timeSet = false;
    } else if (timeElement.isNumber()) {
        _timeSet = true;
        _time = Seconds(timeElement.numberLong());
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kTimeFieldName
                                    << "\" field in response to replSetHeartbeat "
                                       "command to have a numeric type, but found type "
                                    << typeName(timeElement.type()));
    }

    _isReplSet = doc[kIsReplSetFieldName].trueValue();

    Status termStatus = bsonExtractIntegerField(doc, kTermFieldName, &_term);
    if (!termStatus.isOK() && termStatus != ErrorCodes::NoSuchKey) {
        return termStatus;
    }

    Status status = bsonExtractOpTimeField(doc, kDurableOpTimeFieldName, &_durableOpTime);
    if (!status.isOK()) {
        if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    } else {
        _durableOpTimeSet = true;
    }

    // In order to support both the 3.0(V0) and 3.2(V1) heartbeats we must parse the OpTime
    // field based on its type. If it is a Date, we parse it as the timestamp and use
    // initialize's term argument to complete the OpTime type. If it is an Object, then it's
    // V1 and we construct an OpTime out of its nested fields.
    const BSONElement appliedOpTimeElement = doc[kAppliedOpTimeFieldName];
    if (appliedOpTimeElement.eoo()) {
        _appliedOpTimeSet = false;
    } else if (appliedOpTimeElement.type() == bsonTimestamp) {
        _appliedOpTimeSet = true;
        _appliedOpTime = OpTime(appliedOpTimeElement.timestamp(), term);
    } else if (appliedOpTimeElement.type() == Date) {
        _appliedOpTimeSet = true;
        _appliedOpTime = OpTime(Timestamp(appliedOpTimeElement.date()), term);
    } else if (appliedOpTimeElement.type() == Object) {
        Status status = bsonExtractOpTimeField(doc, kAppliedOpTimeFieldName, &_appliedOpTime);
        _appliedOpTimeSet = true;
        // since a v1 OpTime was in the response, the member must be part of a replset
        _isReplSet = true;
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kAppliedOpTimeFieldName
                                    << "\" field in response to replSetHeartbeat "
                                       "command to have type Date or Timestamp, but found type "
                                    << typeName(appliedOpTimeElement.type()));
    }

    const BSONElement electableElement = doc[kIsElectableFieldName];
    if (electableElement.eoo()) {
        _electableSet = false;
    } else {
        _electableSet = true;
        _electable = electableElement.trueValue();
    }

    const BSONElement memberStateElement = doc[kMemberStateFieldName];
    if (memberStateElement.eoo()) {
        _stateSet = false;
    } else if (memberStateElement.type() != NumberInt && memberStateElement.type() != NumberLong) {
        return Status(
            ErrorCodes::TypeMismatch,
            str::stream() << "Expected \"" << kMemberStateFieldName
                          << "\" field in response to replSetHeartbeat "
                             "command to have type NumberInt or NumberLong, but found type "
                          << typeName(memberStateElement.type()));
    } else {
        long long stateInt = memberStateElement.numberLong();
        if (stateInt < 0 || stateInt > MemberState::RS_MAX) {
            return Status(
                ErrorCodes::BadValue,
                str::stream() << "Value for \"" << kMemberStateFieldName
                              << "\" in response to replSetHeartbeat is "
                                 "out of range; legal values are non-negative and no more than "
                              << MemberState::RS_MAX);
        }
        _stateSet = true;
        _state = MemberState(static_cast<int>(stateInt));
    }

    _stateDisagreement = doc[kHasStateDisagreementFieldName].trueValue();


    // Not required for the case of uninitialized members -- they have no config
    const BSONElement configVersionElement = doc[kConfigVersionFieldName];

    // If we have an optime then we must have a configVersion
    if (_appliedOpTimeSet && configVersionElement.eoo()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "Response to replSetHeartbeat missing required \""
                                    << kConfigVersionFieldName
                                    << "\" field even though initialized");
    }

    // If there is a "v" (config version) then it must be an int.
    if (!configVersionElement.eoo() && configVersionElement.type() != NumberInt) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kConfigVersionFieldName
                                    << "\" field in response to replSetHeartbeat to have "
                                       "type NumberInt, but found "
                                    << typeName(configVersionElement.type()));
    }
    _configVersion = configVersionElement.numberInt();

    const BSONElement hbMsgElement = doc[kHbMessageFieldName];
    if (hbMsgElement.eoo()) {
        _hbmsg.clear();
    } else if (hbMsgElement.type() != String) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kHbMessageFieldName
                                    << "\" field in response to replSetHeartbeat to have "
                                       "type String, but found "
                                    << typeName(hbMsgElement.type()));
    } else {
        _hbmsg = hbMsgElement.String();
    }

    const BSONElement syncingToElement = doc[kSyncSourceFieldName];
    if (syncingToElement.eoo()) {
        _syncingTo = HostAndPort();
    } else if (syncingToElement.type() != String) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kSyncSourceFieldName
                                    << "\" field in response to replSetHeartbeat to "
                                       "have type String, but found "
                                    << typeName(syncingToElement.type()));
    } else {
        _syncingTo = HostAndPort(syncingToElement.String());
    }

    const BSONElement rsConfigElement = doc[kConfigFieldName];
    if (rsConfigElement.eoo()) {
        _configSet = false;
        _config = ReplicaSetConfig();
        return Status::OK();
    } else if (rsConfigElement.type() != Object) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected \"" << kConfigFieldName
                                    << "\" in response to replSetHeartbeat to have type "
                                       "Object, but found "
                                    << typeName(rsConfigElement.type()));
    }
    _configSet = true;

    return _config.initialize(rsConfigElement.Obj());
}

MemberState ReplSetHeartbeatResponse::getState() const {
    invariant(_stateSet);
    return _state;
}

Timestamp ReplSetHeartbeatResponse::getElectionTime() const {
    invariant(_electionTimeSet);
    return _electionTime;
}

bool ReplSetHeartbeatResponse::isElectable() const {
    invariant(_electableSet);
    return _electable;
}

Seconds ReplSetHeartbeatResponse::getTime() const {
    invariant(_timeSet);
    return _time;
}

const ReplicaSetConfig& ReplSetHeartbeatResponse::getConfig() const {
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

OpTime ReplSetHeartbeatResponse::getDurableOpTime() const {
    invariant(_durableOpTimeSet);
    return _durableOpTime;
}

}  // namespace repl
}  // namespace mongo
