// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <string_view>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>

namespace mongo {

namespace {

const char kHostFieldName[] = "host";

class CmdDelayMessagesFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, std::mutex* settingsMutex, HostSettingsMap* settings) final {
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
            std::lock_guard<std::mutex> lk(*settingsMutex);
            auto& hostSettings = (*settings)[host];
            hostSettings.state = HostSettings::State::kForward;
            hostSettings.delay = Milliseconds{newDelay};
        }
        return Status::OK();
    }
};

class CmdAcceptConnectionsFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, std::mutex* settingsMutex, HostSettingsMap* settings) final {
        invariant(settingsMutex);
        invariant(settings);

        std::string hostName;
        auto status = bsonExtractStringField(cmdObj, kHostFieldName, &hostName);
        if (!status.isOK()) {
            return status;
        }

        HostAndPort host(hostName);
        {
            std::lock_guard<std::mutex> lk(*settingsMutex);
            auto& hostSettings = (*settings)[host];
            hostSettings.state = HostSettings::State::kForward;
        }
        return Status::OK();
    }
};

class CmdRejectConnectionsFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, std::mutex* settingsMutex, HostSettingsMap* settings) final {
        invariant(settingsMutex);
        invariant(settings);

        std::string hostName;
        auto status = bsonExtractStringField(cmdObj, kHostFieldName, &hostName);
        if (!status.isOK()) {
            return status;
        }

        HostAndPort host(hostName);
        {
            std::lock_guard<std::mutex> lk(*settingsMutex);
            auto& hostSettings = (*settings)[host];
            hostSettings.state = HostSettings::State::kHangUp;
        }
        return Status::OK();
    }
};

class CmdDiscardMessagesFrom final : public BridgeCommand {
public:
    Status run(const BSONObj& cmdObj, std::mutex* settingsMutex, HostSettingsMap* settings) final {
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
            std::lock_guard<std::mutex> lk(*settingsMutex);
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

StatusWith<BridgeCommand*> BridgeCommand::findCommand(std::string_view cmdName) {
    auto it = bridgeCommandMap.find(cmdName);
    if (it != bridgeCommandMap.end()) {
        invariant(it->second);
        return it->second;
    }
    return {ErrorCodes::CommandNotFound, str::stream() << "Unknown command: " << cmdName};
}

BridgeCommand::~BridgeCommand() = default;

}  // namespace mongo
