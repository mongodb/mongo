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
#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

    const std::string kOpTimeFieldName = "opTime";
    const std::string kTimeFieldName = "time";
    const std::string kElectionTimeFieldName = "electionTime";
    const std::string kConfigFieldName = "config";
    const std::string kIsElectableFieldName = "e";
    const std::string kMismatchFieldName = "mismatch";
    const std::string kIsReplSetFieldName = "rs";
    const std::string kHasStateDisagreementFieldName = "stateDisagreement";
    const std::string kMemberStateFieldName = "state";
    const std::string kConfigVersionFieldName = "v";
    const std::string kHbMessageFieldName = "hbmsg";
    const std::string kReplSetFieldName = "set";
    const std::string kSyncSourceFieldName = "syncingTo";

}  // namespace

    ReplSetHeartbeatResponse::ReplSetHeartbeatResponse() :
            _electionTimeSet(false),
            _timeSet(false),
            _time(0),
            _opTimeSet(false),
            _electableSet(false),
            _electable(false),
            _mismatch(false),
            _isReplSet(false),
            _stateDisagreement(false),
            _stateSet(false),
            _version(-1),
            _configSet(false)
            {}

    void ReplSetHeartbeatResponse::addToBSON(BSONObjBuilder* builder) const {
        if (_opTimeSet) {
            builder->appendDate(kOpTimeFieldName, _opTime.asDate());
        }
        if (_timeSet) {
            *builder << kTimeFieldName << _time.total_seconds();
        }
        if (_electionTimeSet) {
            builder->appendDate(kElectionTimeFieldName, _electionTime.asDate());
        }
        if (_configSet) {
            *builder << kConfigFieldName << _config.toBSON();
        }
        if (_electableSet) {
            *builder << kIsElectableFieldName << _electable;
        }
        if (_mismatch) {
            *builder << kMismatchFieldName << _mismatch;
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
        if (_version != -1) {
            *builder << kConfigVersionFieldName << _version;
        }
        *builder << kHbMessageFieldName << _hbmsg;
        if (!_setName.empty()) {
            *builder << kReplSetFieldName << _setName;
        }
        if (!_syncingTo.empty()) {
            *builder << kSyncSourceFieldName << _syncingTo;
        }
    }

    BSONObj ReplSetHeartbeatResponse::toBSON() const {
        BSONObjBuilder builder;
        addToBSON(&builder);
        return builder.obj();
    }

    Status ReplSetHeartbeatResponse::initialize(const BSONObj& doc) {
        const BSONElement electionTimeElement = doc[kElectionTimeFieldName];
        if (electionTimeElement.eoo()) {
            _electionTimeSet = false;
        }
        else if (electionTimeElement.type() == Timestamp) {
            _electionTimeSet = true;
            _electionTime = electionTimeElement._opTime();
        }
        else if (electionTimeElement.type() == Date) {
            _electionTime = true;
            _electionTime = OpTime(electionTimeElement.date());
        }
        else {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kElectionTimeFieldName << "\" field in response to replSetHeartbeat "
                          "command to have type Date or Timestamp, but found type " <<
                          typeName(electionTimeElement.type()));
        }

        const BSONElement timeElement = doc[kTimeFieldName];
        if (timeElement.eoo()) {
            _timeSet = false;
        }
        else if (timeElement.isNumber()) {
            _timeSet = true;
            _time = Seconds(timeElement.numberLong());
        }
        else {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kTimeFieldName << "\" field in reponse to replSetHeartbeat "
                          "command to have a numeric type, but found type " <<
                          typeName(timeElement.type()));
        }

        const BSONElement opTimeElement = doc[kOpTimeFieldName];
        if (opTimeElement.eoo()) {
            _opTimeSet = false;
        }
        else if (opTimeElement.type() == Timestamp) {
            _opTimeSet = true;
            _opTime = opTimeElement._opTime();
        }
        else if (opTimeElement.type() == Date) {
            _opTimeSet = true;
            _opTime = OpTime(opTimeElement.date());
        }
        else {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kOpTimeFieldName << "\" field in response to replSetHeartbeat "
                          "command to have type Date or Timestamp, but found type " <<
                          typeName(opTimeElement.type()));
        }

        const BSONElement electableElement = doc[kIsElectableFieldName];
        if (electableElement.eoo()) {
            _electableSet = false;
        }
        else {
            _electableSet = true;
            _electable = electableElement.trueValue();
        }

        _mismatch = doc[kMismatchFieldName].trueValue();
        _isReplSet = doc[kIsReplSetFieldName].trueValue();

        const BSONElement memberStateElement = doc[kMemberStateFieldName];
        if (memberStateElement.eoo()) {
            _stateSet = false;
        }
        else if (memberStateElement.type() != NumberInt &&
                 memberStateElement.type() != NumberLong) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kMemberStateFieldName << "\" field in response to replSetHeartbeat "
                          " command to have type NumberInt or NumberLong, but found type " <<
                          typeName(memberStateElement.type()));
        }
        else {
            long long stateInt = memberStateElement.numberLong();
            if (stateInt < 0 || stateInt > MemberState::RS_MAX) {
                return Status(ErrorCodes::BadValue, str::stream() << "Value for \"" <<
                              kMemberStateFieldName << "\" in response to replSetHeartbeat is "
                              " out of range; legal values are non-negative and no more than " <<
                              MemberState::RS_MAX);
            }
            _state = MemberState(static_cast<int>(stateInt));
        }

        _stateDisagreement = doc[kHasStateDisagreementFieldName].trueValue();

        const BSONElement versionElement = doc[kConfigVersionFieldName];
        if (versionElement.eoo()) {
            return Status(ErrorCodes::NoSuchKey, str::stream() <<
                          "Response to replSetHeartbeat missing required \"" <<
                          kConfigVersionFieldName << " field");
        }
        if (versionElement.type() != NumberInt) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kConfigVersionFieldName <<
                          "\" field in response to replSetHeartbeat to have "
                          "type NumberInt, but found " << typeName(versionElement.type()));
        }
        _version = versionElement.numberInt();

        const BSONElement replSetNameElement = doc[kReplSetFieldName];
        if (replSetNameElement.eoo()) {
            return Status(ErrorCodes::NoSuchKey, str::stream() <<
                          "Response to replSetHeartbeat missing required \"" <<
                          kReplSetFieldName << "\" field");
        }
        if (replSetNameElement.type() != String) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kReplSetFieldName << "\" field in response to replSetHeartbeat to have "
                          "type String, but found " << typeName(replSetNameElement.type()));
        }
        _setName = replSetNameElement.String();

        const BSONElement hbMsgElement = doc[kHbMessageFieldName];
        if (hbMsgElement.eoo()) {
            _hbmsg.clear();
        }
        else if (hbMsgElement.type() != String) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kHbMessageFieldName << "\" field in response to replSetHeartbeat to have "
                          "type String, but found " << typeName(hbMsgElement.type()));
        }
        _hbmsg = hbMsgElement.String();

        const BSONElement syncingToElement = doc[kSyncSourceFieldName];
        if (syncingToElement.eoo()) {
            _syncingTo.clear();
        }
        else if (syncingToElement.type() != String) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kSyncSourceFieldName << "\" field in response to replSetHeartbeat to "
                          "have type String, but found " << typeName(syncingToElement.type()));
        }
        _syncingTo = syncingToElement.String();

        const BSONElement rsConfigElement = doc[kConfigFieldName];
        if (rsConfigElement.eoo()) {
            _configSet = false;
            _config = ReplicaSetConfig();
        }
        else if (rsConfigElement.type() != Object) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kConfigFieldName << "\" in response to replSetHeartbeat to have type "
                          "Object, but found " << typeName(rsConfigElement.type()));
        }
        _configSet = true;
        return _config.initialize(rsConfigElement.Obj());
    }

    MemberState ReplSetHeartbeatResponse::getState() const {
        invariant(_stateSet);
        return _state;
    }

    OpTime ReplSetHeartbeatResponse::getElectionTime() const {
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

    OpTime ReplSetHeartbeatResponse::getOpTime() const {
        invariant(_opTimeSet);
        return _opTime;
    }

    const ReplicaSetConfig& ReplSetHeartbeatResponse::getConfig() const {
        invariant(_configSet);
        return _config;
    }

} // namespace repl
} // namespace mongo
