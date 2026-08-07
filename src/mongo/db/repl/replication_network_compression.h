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

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
class TenantId;
class MessageCompressorManager;

namespace repl {

/*
 * Parsed form of replicationNetworkCompression / replication.networkCompression.compressors.
 *
 *   inheritProcessDefault == true  -> inherit net.compression.compressors (default empty value).
 *   disabled == true               -> negotiate replication data-plane connections uncompressed.
 *   otherwise                      -> use allowList as the ordered per-session compressor list.
 */
struct ReplicationNetworkCompressionSetting {
    bool inheritProcessDefault{true};
    bool disabled{false};
    std::vector<std::string> allowList;
};

/*
 * Parses a replicationNetworkCompression value. Accepts:
 *   - ""          -> inherit net.compression.compressors
 *   - "disabled"  -> negotiate uncompressed (case-insensitive)
 *   - "a,b,c"     -> ordered allow-list, with whitespace trimmed and empty entries ignored
 *
 * A non-empty value containing only separators/whitespace is rejected. Compressor availability is
 * checked later during startup finalization, not by this parser.
 */
MONGO_MOD_PUBLIC Status parseReplicationNetworkCompression(
    std::string_view value, ReplicationNetworkCompressionSetting* out);

/*
 * IDL validator callback. Delegates to parseReplicationNetworkCompression().
 */
MONGO_MOD_PUBLIC Status validateReplicationNetworkCompression(
    std::string_view value, const boost::optional<TenantId>& tenantId);

/*
 * Loads the current value of the replicationNetworkCompression server parameter and returns
 * it in parsed form. Thread-safe.
 */
MONGO_MOD_PUBLIC ReplicationNetworkCompressionSetting getReplicationNetworkCompressionSetting();

/*
 * Applies the node-local replicationNetworkCompression setting to a client-side
 * MessageCompressorManager before DBClientConnection sends hello. Used by replication data-plane
 * clients such as the oplog fetcher, initial-sync collection cloner, and rollback remote oplog
 * reader.
 *
 * Must be called by the connection's (re)connect thread before the handshake. Reapply on reconnect
 * because DBClientConnection reuses the same manager and these per-session setters are not
 * internally synchronized.
 */
MONGO_MOD_PUBLIC void applyReplicationNetworkCompressionToManager(MessageCompressorManager& mgr);

}  // namespace repl
}  // namespace mongo
