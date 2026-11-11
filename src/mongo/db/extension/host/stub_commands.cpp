// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/stub_commands.h"

#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {
namespace {
StringMap<std::string>& stubCommands() {
    static StringMap<std::string> stubs;
    return stubs;
}
}  // namespace

void registerStubCommand(std::string commandName, std::string message) {
    LOGV2(13730205,
          "Registering stub for extension command",
          "commandName"_attr = commandName,
          "message"_attr = message);

    // Silently skip registration if a stub has already been registered. The first registration gets
    // priority.
    stubCommands().try_emplace(std::move(commandName), std::move(message));
}

void unregisterStubCommand_forTest(std::string_view commandName) {
    auto& stubs = stubCommands();
    const auto it = stubs.find(commandName);
    if (it != stubs.end()) {
        stubs.erase(it);
    }
}

boost::optional<Status> getStubCommandError(std::string_view commandName) {
    const auto& stubs = stubCommands();
    auto it = stubs.find(commandName);
    if (it == stubs.end()) {
        return boost::none;
    }
    return Status(ErrorCodes::Error(13730200), it->second);
}
}  // namespace mongo::extension::host
