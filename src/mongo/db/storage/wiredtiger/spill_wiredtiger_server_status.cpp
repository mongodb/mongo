/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/spill_wiredtiger_server_status.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {

namespace {
const std::vector<std::string>& fieldsToInclude = {
    "block-manager: blocks read",
    "block-manager: blocks written",
    "block-manager: bytes read",
    "block-manager: bytes written",
    "cache: application thread time evicting (usecs)",
    "cache: application threads eviction requested with cache fill ratio < 25%",
    "cache: application threads eviction requested with cache fill ratio >= 75%",
    "cache: application threads page write from cache to disk count",
    "cache: application threads page write from cache to disk time (usecs)",
    "cache: bytes allocated for updates",
    "cache: bytes currently in the cache",
    "cache: bytes read into cache",
    "cache: bytes written from cache",
    "cache: eviction currently operating in aggressive mode",
    "cache: eviction empty score",
    "cache: eviction state",
    "cache: eviction walk target strategy clean pages",
    "cache: eviction walk target strategy dirty pages",
    "cache: eviction walk target strategy pages with updates",
    "cache: forced eviction - pages evicted that were clean count",
    "cache: forced eviction - pages evicted that were dirty count",
    "cache: forced eviction - pages selected count",
    "cache: forced eviction - pages selected unable to be evicted count",
    "cache: hazard pointer blocked page eviction",
    "cache: maximum bytes configured",
    "cache: maximum page size seen at eviction",
    "cache: number of times dirty trigger was reached",
    "cache: number of times eviction trigger was reached",
    "cache: number of times updates trigger was reached",
    "cache: page evict attempts by application threads",
    "cache: page evict failures by application threads",
    "cache: pages queued for eviction",
    "cache: pages queued for urgent eviction",
    "cache: tracked dirty bytes in the cache",
    "session: open session count",
};
}

bool SpillWiredTigerServerStatusSection::includeByDefault() const {
    return true;
}

/**
 * Verbosity levels of WiredTiger stats included in serverStatus is set through serverStatus
 * calls as db.serverStatus({spillWiredTiger: <verbosity>}). We can configure FTDC verbosity
 * levels using the spillWiredTigerServerStatusVerbosity server parameter.
 *
 * The 3 verbosity levels are:
 *    - Level 0: No WiredTiger stats included
 *    - Level 1: Select WiredTiger stats included (default)
 *    - Level 2: All WiredTiger stats included
 */
BSONObj SpillWiredTigerServerStatusSection::generateSection(
    OperationContext* opCtx, const BSONElement& configElement) const {
    SpillWiredTigerKVEngine* engine = checked_cast<SpillWiredTigerKVEngine*>(
        opCtx->getServiceContext()->getStorageEngine()->getSpillEngine());
    BSONObjBuilder bob;

    bob.append("storageSize", [engine] {
        try {
            return engine->storageSize(*engine->newRecoveryUnit());
        } catch (const DBException&) {
            return int64_t{0};
        }
    }());

    int verbosity = configElement.isNumber() ? configElement.safeNumberInt() : 1;
    switch (verbosity) {
        case 0:
            break;
        case 1: {
            if (!WiredTigerUtil::collectConnectionStatistics(*engine, bob, fieldsToInclude)) {
                LOGV2_DEBUG(10641700, 2, "Spill WiredTiger is not ready to collect statistics.");
            }
            break;
        }
        case 2: {
            if (!WiredTigerUtil::collectConnectionStatistics(*engine, bob)) {
                LOGV2_DEBUG(10641701, 2, "Spill WiredTiger is not ready to collect statistics.");
            }
            break;
        }
    }
    return bob.obj();
}
}  // namespace mongo
