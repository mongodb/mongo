// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

enum class EventType : uint8_t {
    kRequest = 0,       // A user issued command.
    kResponse = 1,      // A response, generated for a user command.
    kSessionStart = 2,  // A non-message event indicating the start of a session.
    kSessionEnd = 3,    // A non-message event indicating the end of a session.

    kMax,  // Not a valid event type, used to check values are in-range.
};
}
