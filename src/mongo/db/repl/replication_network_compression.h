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
class BSONObjBuilder;
class TenantId;
class MessageCompressorManager;

namespace repl {

/*
 * Parsed replicationNetworkCompression value. If neither flag is set, allowList holds the
 * compressors in preference order.
 */
struct ReplicationNetworkCompressionSetting {
    bool inheritProcessDefault{true};
    bool disabled{false};
    std::vector<std::string> allowList;
};

/*
 * Parses "" (use net.compression.compressors), "disabled" (case-insensitive), or a comma-separated
 * list of compressor names. Whether the compressors are built in is checked at startup, not here.
 */
MONGO_MOD_PUBLIC Status parseReplicationNetworkCompression(
    std::string_view value, ReplicationNetworkCompressionSetting* out);

MONGO_MOD_PUBLIC Status validateReplicationNetworkCompression(
    std::string_view value, const boost::optional<TenantId>& tenantId);

MONGO_MOD_PUBLIC ReplicationNetworkCompressionSetting getReplicationNetworkCompressionSetting();

/*
 * Configures a replication client connection (oplog fetcher, initial sync cloner, rollback). Call
 * before every connect, because the manager is reused across reconnects.
 */
MONGO_MOD_PUBLIC void applyReplicationNetworkCompressionToManager(MessageCompressorManager& mgr);

/*
 * Server side of hello. 'isReplicationClient' comes from the client and is not authenticated, so
 * it only chooses the compression policy and the repl.compression counters.
 */
MONGO_MOD_PUBLIC void negotiateHelloCompression(
    MessageCompressorManager& mgr,
    bool isReplicationClient,
    const boost::optional<std::vector<std::string_view>>& clientCompressors,
    BSONObjBuilder* result);

}  // namespace repl
}  // namespace mongo
