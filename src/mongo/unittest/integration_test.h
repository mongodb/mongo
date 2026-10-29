// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace unittest {

/**
 * Gets the connection string for the MongoDB deployment that this test is running
 * against.
 */
ConnectionString getFixtureConnectionString();

/**
 * Determines if the integration test is configured to use gRPC.
 */
bool shouldUseGRPCEgress();

}  // namespace unittest
}  // namespace mongo
