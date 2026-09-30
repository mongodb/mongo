// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/session.h"
#include "mongo/util/modules.h"
/*
 * Specifies the purpose of a connection. Currently used to collect  unique metrics on
 * replication connections.
 */
namespace [[MONGO_MOD_PUBLIC]] mongo {
enum class ConnectionPurpose { kDefault = 0, kReplication = 1 };
ConnectionPurpose& getConnectionPurpose(transport::Session* session);
}  // namespace mongo
