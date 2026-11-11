// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

/**
 * Stub errors for commands provided by extensions that are not loaded, read from the "stubCommands"
 * section of the extension stub file. A stub is only consulted when the command registry for the
 * service handling the request has no command with that name.
 */
namespace mongo::extension::host {

/**
 * Registers a stub for 'commandName' that fails with 'message'. If a stub is already registered
 * for 'commandName', this does nothing.
 */
void registerStubCommand(std::string commandName, std::string message);

/**
 * Removes the stub registered for 'commandName', if any. For test use only.
 */
void unregisterStubCommand_forTest(std::string_view commandName);

/**
 * Returns an error with the stub's message if a stub is registered for 'commandName', and
 * boost::none otherwise.
 */
[[MONGO_MOD_PUBLIC]] boost::optional<Status> getStubCommandError(std::string_view commandName);
}  // namespace mongo::extension::host
