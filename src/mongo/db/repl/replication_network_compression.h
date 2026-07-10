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
 * Parsed form of the replicationNetworkCompression setParameter /
 * replication.networkCompression.compressors YAML value.
 *
 *   inheritProcessDefault == true  -> caller must not touch the manager (advertise the
 *                                     process-wide net.compression.compressors list). This is the
 *                                     default (empty value); inheriting net keeps replication
 *                                     compression unchanged on upgrade.
 *   inheritProcessDefault == false && disabled == true
 *                                  -> caller must call disableCompressionForThisSession()
 *                                     and NOT set an allow-list.
 *   inheritProcessDefault == false && disabled == false
 *                                  -> caller must clear disable state and pass allowList as
 *                                     the per-session allow-list. Order is preserved.
 */
struct ReplicationNetworkCompressionSetting {
    bool inheritProcessDefault{true};
    bool disabled{false};
    std::vector<std::string> allowList;
};

/*
 * Parses a replicationNetworkCompression value. Accepts:
 *   - ""          -> inheritProcessDefault=true
 *   - "disabled"  -> inheritProcessDefault=false, disabled=true (case-insensitive)
 *   - "a,b,c"     -> inheritProcessDefault=false, allowList={"a","b","c"}
 *                    (trimmed, non-empty entries only)
 *
 * Returns a non-OK Status only for structurally invalid input. Empty segments in an otherwise
 * non-empty list are tolerated (for example "snappy,"), but a non-empty value that consists only
 * of separators/whitespace (for example ",") is rejected rather than silently treated as inherit.
 * Individual compressor names are NOT validated against the process-wide registry here; startup
 * folds this list into the registry union and finalizeSupportedCompressors() performs the same
 * fail-fast availability check as net.compression.compressors.
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
 * MessageCompressorManager (i.e. the manager owned by a DBClientConnection used to talk to the
 * sync source) right before its "hello" handshake runs. This is the single source of truth for
 * the client side of SERVER-130410 and is shared by every replication client connection so their
 * compressor negotiation cannot drift:
 *
 *   - the steady-state / initial-sync oplog fetcher (OplogFetcher::_connect),
 *   - the initial-sync collection cloner connection (AllDatabaseCloner::connectStage), and
 *   - the rollback remote oplog connection (BackgroundSync::_runRollback).
 *
 * The mapping is:
 *   inheritProcessDefault -> enableCompressionForThisSession() + clear allow-list (advertise the
 *                            process-wide net.compression.compressors list);
 *   disabled              -> disableCompressionForThisSession() + clear allow-list (negotiate
 *                            uncompressed regardless of net.compression.compressors);
 *   otherwise             -> enableCompressionForThisSession() + set the allow-list (advertise the
 *                            operator-chosen subset, intersected with the process-wide capability
 *                            set inside clientBegin()).
 *
 * MUST be called before the handshake is sent, and only from the thread that drives that
 * connection's (re)connect (the manager setters are not internally synchronized). Because the
 * same manager instance is reused across DBClientConnection auto-reconnects, re-invoking this on
 * each (re)connect keeps the manager's state consistent with the stored setting; the setting
 * itself is startup-only, so on a running mongod this simply re-applies the fixed configuration.
 * This is purely a client-side decision and requires no server-side change.
 */
MONGO_MOD_PUBLIC void applyReplicationNetworkCompressionToManager(MessageCompressorManager& mgr);

}  // namespace repl
}  // namespace mongo
