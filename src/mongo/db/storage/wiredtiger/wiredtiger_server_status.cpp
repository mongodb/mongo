/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <string>
#include <vector>

#include <wiredtiger.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC


namespace mongo {

using std::string;

bool WiredTigerServerStatusSection::includeByDefault() const {
    return true;
}

BSONObj WiredTigerServerStatusSection::generateSection(OperationContext* opCtx,
                                                       const BSONElement& configElement) const {
    Lock::GlobalLock lk(opCtx,
                        LockMode::MODE_IS,
                        Date_t::now(),
                        Lock::InterruptBehavior::kLeaveUnlocked,
                        // Replication state change does not affect the following operation and we
                        // don't need a flow control ticket.
                        [] {
                            Lock::GlobalLockSkipOptions options;
                            options.skipRSTLLock = true;
                            return options;
                        }());
    if (!lk.isLocked()) {
        LOGV2_DEBUG(3088800, 2, "Failed to retrieve wiredTiger statistics");
        return BSONObj();
    }

    // The session does not open a transaction here as one is not needed and opening one would
    // mean that execution could become blocked when a new transaction cannot be allocated
    // immediately.
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    invariant(session);

    WT_SESSION* s = session->getSession();
    invariant(s);
    const string uri = "statistics:";

    // Filter out unrelevant statistic fields.
    std::vector<std::string> fieldsToIgnore = {"LSM"};

    BSONObjBuilder bob;
    Status status =
        WiredTigerUtil::exportTableToBSON(s, uri, "statistics=(fast)", &bob, fieldsToIgnore);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }

    WiredTigerKVEngine::appendGlobalStats(opCtx, bob);

    WiredTigerKVEngine* engine = checked_cast<WiredTigerKVEngine*>(
        opCtx->getServiceContext()->getStorageEngine()->getEngine());
    WiredTigerUtil::appendSnapshotWindowSettings(engine, session, &bob);

    {
        BSONObjBuilder subsection(bob.subobjStart("oplog"));
        subsection.append("visibility timestamp",
                          Timestamp(engine->getOplogManager()->getOplogReadTimestamp()));
    }

    return bob.obj();
}

}  // namespace mongo
