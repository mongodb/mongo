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

#include "mongo/tools/mongobridge_tool/bridge_commands.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <limits>
#include <mutex>
#include <string>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>

namespace mongo {

namespace {

const char kHostFieldName[] = "host";

class CmdDelayMessagesFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, stdx::mutex* settingsMutex, HostSettingsMap* settings) final {
        invariant(settingsMutex);
        invariant(settings);

        std::string hostName;
        auto status = bsonExtractStringField(cmdObj, kHostFieldName, &hostName);
        if (!status.isOK()) {
            return status;
        }

        long long newDelay;
        status = bsonExtractIntegerField(cmdObj, "delay", &newDelay);
        if (!status.isOK()) {
            return status;
        }
        if (newDelay < 0 || newDelay > std::numeric_limits<int>::max()) {
            return {ErrorCodes::BadValue, "'delay' field must be a nonnegative integer"};
        }

        HostAndPort host(hostName);
        {
            stdx::lock_guard<stdx::mutex> lk(*settingsMutex);
            auto& hostSettings = (*settings)[host];
            hostSettings.state = HostSettings::State::kForward;
            hostSettings.delay = Milliseconds{newDelay};
        }
        return Status::OK();
    }
};

class CmdAcceptConnectionsFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, stdx::mutex* settingsMutex, HostSettingsMap* settings) final {
        invariant(settingsMutex);
        invariant(settings);

        std::string hostName;
        auto status = bsonExtractStringField(cmdObj, kHostFieldName, &hostName);
        if (!status.isOK()) {
            return status;
        }

        HostAndPort host(hostName);
        {
            stdx::lock_guard<stdx::mutex> lk(*settingsMutex);
            auto& hostSettings = (*settings)[host];
            hostSettings.state = HostSettings::State::kForward;
        }
        return Status::OK();
    }
};

class CmdRejectConnectionsFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, stdx::mutex* settingsMutex, HostSettingsMap* settings) final {
        invariant(settingsMutex);
        invariant(settings);

        std::string hostName;
        auto status = bsonExtractStringField(cmdObj, kHostFieldName, &hostName);
        if (!status.isOK()) {
            return status;
        }

        HostAndPort host(hostName);
        {
            stdx::lock_guard<stdx::mutex> lk(*settingsMutex);
            auto& hostSettings = (*settings)[host];
            hostSettings.state = HostSettings::State::kHangUp;
        }
        return Status::OK();
    }
};

class CmdDiscardMessagesFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, stdx::mutex* settingsMutex, HostSettingsMap* settings) final {
        invariant(settingsMutex);
        invariant(settings);

        std::string hostName;
        auto status = bsonExtractStringField(cmdObj, kHostFieldName, &hostName);
        if (!status.isOK()) {
            return status;
        }

        double newLoss;
        auto lossElem = cmdObj["loss"];
        if (lossElem) {
            if (!lossElem.isNumber()) {
                return {ErrorCodes::TypeMismatch, "'loss' field must be a number"};
            }

            newLoss = lossElem.numberDouble();
            if (newLoss < 0.0 || newLoss > 1.0) {
                return {ErrorCodes::BadValue, "'loss' field must be a number between 0 and 1"};
            }
        } else {
            return {ErrorCodes::NoSuchKey, "Missing required field 'loss'"};
        }

        HostAndPort host(hostName);
        {
            stdx::lock_guard<stdx::mutex> lk(*settingsMutex);
            auto& hostSettings = (*settings)[host];
            hostSettings.state = HostSettings::State::kDiscard;
            hostSettings.loss = newLoss;
        }
        return Status::OK();
    }
};

StringMap<BridgeCommand*> bridgeCommandMap;

MONGO_INITIALIZER(RegisterBridgeCommands)(InitializerContext* context) {
    bridgeCommandMap["delayMessagesFrom"] = new CmdDelayMessagesFrom();
    bridgeCommandMap["acceptConnectionsFrom"] = new CmdAcceptConnectionsFrom();
    bridgeCommandMap["rejectConnectionsFrom"] = new CmdRejectConnectionsFrom();
    bridgeCommandMap["discardMessagesFrom"] = new CmdDiscardMessagesFrom();
}

}  // namespace

StatusWith<BridgeCommand*> BridgeCommand::findCommand(StringData cmdName) {
    auto it = bridgeCommandMap.find(cmdName);
    if (it != bridgeCommandMap.end()) {
        invariant(it->second);
        return it->second;
    }
    return {ErrorCodes::CommandNotFound, str::stream() << "Unknown command: " << cmdName};
}

BridgeCommand::~BridgeCommand() = default;

}  // namespace mongo
