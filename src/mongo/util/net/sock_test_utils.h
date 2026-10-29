// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/net/sock.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
using SocketPtr = std::shared_ptr<Socket>;
using SocketPair = std::pair<SocketPtr, SocketPtr>;

// Create a connected pair of sockets for testing purposes.
SocketPair socketPair(int type, int protocol = 0);

}  // namespace mongo
