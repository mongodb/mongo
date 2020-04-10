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

#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

namespace {

const std::string kCheckEmptyFieldName = "checkEmpty";
const std::string kConfigVersionFieldName = "configVersion";
const std::string kConfigTermFieldName = "configTerm";
const std::string kHeartbeatVersionFieldName = "hbv";
const std::string kSenderHostFieldName = "from";
const std::string kSenderIdFieldName = "fromId";
const std::string kSetNameFieldName = "replSetHeartbeat";
const std::string kTermFieldName = "term";
}  // namespace

Status ReplSetHeartbeatArgsV1::initialize(const BSONObj& argsObj) {
    Status status =
        bsonExtractBooleanFieldWithDefault(argsObj, kCheckEmptyFieldName, false, &_checkEmpty);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kConfigVersionFieldName, &_configVersion);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerFieldWithDefault(
        argsObj, kConfigTermFieldName, OpTime::kUninitializedTerm, &_configTerm);
    if (!status.isOK())
        return status;

    long long tempHeartbeatVersion;
    status = bsonExtractIntegerField(argsObj, kHeartbeatVersionFieldName, &tempHeartbeatVersion);
    if (status.isOK()) {
        if (tempHeartbeatVersion != 1) {
            return Status(ErrorCodes::Error(40666),
                          str::stream()
                              << "Found invalid value for field " << kHeartbeatVersionFieldName
                              << ": " << tempHeartbeatVersion);
        }
        _heartbeatVersion = tempHeartbeatVersion;
        _hasHeartbeatVersion = true;
    } else if (status.code() != ErrorCodes::NoSuchKey) {
        return status;
    }

    status = bsonExtractIntegerFieldWithDefault(argsObj, kSenderIdFieldName, -1, &_senderId);
    if (!status.isOK())
        return status;

    std::string hostAndPortString;
    status = bsonExtractStringField(argsObj, kSenderHostFieldName, &hostAndPortString);
    if (!status.isOK())
        return status;
    if (!hostAndPortString.empty()) {
        status = _senderHost.initialize(hostAndPortString);
        if (!status.isOK())
            return status;
        _hasSender = true;
    }

    status = bsonExtractIntegerField(argsObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    status = bsonExtractStringField(argsObj, kSetNameFieldName, &_setName);
    if (!status.isOK())
        return status;

    return Status::OK();
}

bool ReplSetHeartbeatArgsV1::isInitialized() const {
    return _configVersion != -1 && _term != -1 && !_setName.empty();
}

void ReplSetHeartbeatArgsV1::setConfigVersion(long long newVal) {
    _configVersion = newVal;
}

void ReplSetHeartbeatArgsV1::setConfigTerm(long long newVal) {
    _configTerm = newVal;
}

void ReplSetHeartbeatArgsV1::setHeartbeatVersion(long long newVal) {
    _heartbeatVersion = newVal;
    _hasHeartbeatVersion = true;
}

void ReplSetHeartbeatArgsV1::setSenderHost(const HostAndPort& newVal) {
    _senderHost = newVal;
    _hasSender = true;
}

void ReplSetHeartbeatArgsV1::setSenderId(long long newVal) {
    _senderId = newVal;
}

void ReplSetHeartbeatArgsV1::setSetName(const std::string& newVal) {
    _setName = newVal;
}

void ReplSetHeartbeatArgsV1::setTerm(long long newVal) {
    _term = newVal;
}

void ReplSetHeartbeatArgsV1::setCheckEmpty() {
    _checkEmpty = true;
}

BSONObj ReplSetHeartbeatArgsV1::toBSON() const {
    invariant(isInitialized());
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.obj();
}

void ReplSetHeartbeatArgsV1::addToBSON(BSONObjBuilder* builder) const {
    builder->append(kSetNameFieldName, _setName);
    if (_checkEmpty) {
        builder->append(kCheckEmptyFieldName, _checkEmpty);
    }
    builder->appendIntOrLL(kConfigVersionFieldName, _configVersion);
    builder->appendIntOrLL(kConfigTermFieldName, _configTerm);
    if (_hasHeartbeatVersion) {
        builder->appendIntOrLL(kHeartbeatVersionFieldName, _hasHeartbeatVersion);
    }
    builder->append(kSenderHostFieldName, _hasSender ? _senderHost.toString() : "");
    builder->appendIntOrLL(kSenderIdFieldName, _senderId);
    builder->appendIntOrLL(kTermFieldName, _term);
}

}  // namespace repl
}  // namespace mongo
