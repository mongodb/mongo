// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/dbmessage.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/protocol.h"

namespace mongo::transport {

/** Build a `DbResponse` carrying a rate-limit-rejection error for `message`. */
DbResponse makeDbResponseErrorForRateLimiting(const Message& message);

}  // namespace mongo::transport
