/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
MONGO_MOD_PUBLIC void recordOplogBytesReceived(int64_t bytes);
MONGO_MOD_PUBLIC void recordOplogBytesSent(int64_t bytes);

}  // namespace mongo::repl
