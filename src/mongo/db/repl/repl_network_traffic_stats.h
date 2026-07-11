// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::repl {

/**
 * Records oplog bytes transferred over the network for replication, surfaced as the `serverStatus`
 * fields metrics.repl.network.bytes (received) and metrics.repl.network.bytesSent (sent), and as
 * OpenTelemetry counters. Both values are the raw (uncompressed) size of the oplog entries, not the
 * on-wire size after compression, and together they describe oplog network traffic in both
 * directions for a node.
 *
 * Call recordOplogBytesReceived() to record oplog bytes fetched from a node's sync source.
 * Call recordOplogBytesSent() to record oplog bytes served to a node syncing from it.
 *
 * `bytes` must be non-negative; both helpers are thread-safe.
 */
[[MONGO_MOD_PUBLIC]] void recordOplogBytesReceived(int64_t bytes);
[[MONGO_MOD_PUBLIC]] void recordOplogBytesSent(int64_t bytes);

}  // namespace mongo::repl
