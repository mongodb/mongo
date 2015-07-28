/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/repl_set_heartbeat_args.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

namespace {

const std::string kCheckEmptyFieldName = "checkEmpty";
const std::string kProtocolVersionFieldName = "pv";
const std::string kConfigVersionFieldName = "v";
const std::string kSenderIdFieldName = "fromId";
const std::string kSetNameFieldName = "replSetHeartbeat";
const std::string kSenderHostFieldName = "from";

const std::string kLegalHeartbeatFieldNames[] = {kCheckEmptyFieldName,
                                                 kProtocolVersionFieldName,
                                                 kConfigVersionFieldName,
                                                 kSenderIdFieldName,
                                                 kSetNameFieldName,
                                                 kSenderHostFieldName};

}  // namespace

ReplSetHeartbeatArgs::ReplSetHeartbeatArgs()
    : _hasCheckEmpty(false),
      _hasProtocolVersion(false),
      _hasConfigVersion(false),
      _hasSenderId(false),
      _hasSetName(false),
      _hasSenderHost(false),
      _checkEmpty(false),
      _protocolVersion(-1),
      _configVersion(-1),
      _senderId(-1),
      _setName(""),
      _senderHost(HostAndPort()) {}

Status ReplSetHeartbeatArgs::initialize(const BSONObj& argsObj) {
    Status status =
        bsonCheckOnlyHasFields("ReplSetHeartbeatArgs", argsObj, kLegalHeartbeatFieldNames);
    if (!status.isOK())
        return status;

    status = bsonExtractBooleanFieldWithDefault(argsObj, kCheckEmptyFieldName, false, &_checkEmpty);
    if (!status.isOK())
        return status;
    _hasCheckEmpty = true;

    status = bsonExtractIntegerField(argsObj, kProtocolVersionFieldName, &_protocolVersion);
    if (!status.isOK())
        return status;
    _hasProtocolVersion = true;

    status = bsonExtractIntegerField(argsObj, kConfigVersionFieldName, &_configVersion);
    if (!status.isOK())
        return status;
    _hasConfigVersion = true;

    status = bsonExtractIntegerFieldWithDefault(argsObj, kSenderIdFieldName, -1, &_senderId);
    if (!status.isOK())
        return status;
    _hasSenderId = true;

    status = bsonExtractStringField(argsObj, kSetNameFieldName, &_setName);
    if (!status.isOK())
        return status;
    _hasSetName = true;

    std::string hostAndPortString;
    status =
        bsonExtractStringFieldWithDefault(argsObj, kSenderHostFieldName, "", &hostAndPortString);
    if (!status.isOK())
        return status;

    if (!hostAndPortString.empty()) {
        status = _senderHost.initialize(hostAndPortString);
        if (!status.isOK())
            return status;
        _hasSenderHost = true;
    }

    return Status::OK();
}

bool ReplSetHeartbeatArgs::isInitialized() const {
    return _hasProtocolVersion && _hasConfigVersion && _hasSetName;
}

BSONObj ReplSetHeartbeatArgs::toBSON() const {
    invariant(isInitialized());
    BSONObjBuilder builder;
    builder.append("replSetHeartbeat", _setName);
    builder.appendIntOrLL("pv", _protocolVersion);
    builder.appendIntOrLL("v", _configVersion);
    builder.append("from", _hasSenderHost ? _senderHost.toString() : "");

    if (_hasSenderId) {
        builder.appendIntOrLL("fromId", _senderId);
    }
    if (_hasCheckEmpty) {
        builder.append("checkEmpty", _checkEmpty);
    }
    return builder.obj();
}

void ReplSetHeartbeatArgs::setCheckEmpty(bool newVal) {
    _checkEmpty = newVal;
    _hasCheckEmpty = true;
}

void ReplSetHeartbeatArgs::setProtocolVersion(long long newVal) {
    _protocolVersion = newVal;
    _hasProtocolVersion = true;
}

void ReplSetHeartbeatArgs::setConfigVersion(long long newVal) {
    _configVersion = newVal;
    _hasConfigVersion = true;
}

void ReplSetHeartbeatArgs::setSenderId(long long newVal) {
    _senderId = newVal;
    _hasSenderId = true;
}

void ReplSetHeartbeatArgs::setSetName(std::string newVal) {
    _setName = newVal;
    _hasSetName = true;
}

void ReplSetHeartbeatArgs::setSenderHost(HostAndPort newVal) {
    _senderHost = newVal;
    _hasSenderHost = true;
}

}  // namespace repl
}  // namespace mongo
