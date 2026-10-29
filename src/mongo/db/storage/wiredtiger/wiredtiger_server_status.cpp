// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"

#include <wiredtiger.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC


namespace mongo {

bool WiredTigerServerStatusSection::includeByDefault() const {
    return true;
}

BSONObj WiredTigerServerStatusSection::generateSection(OperationContext* opCtx,
                                                       const BSONElement& configElement) const {
    WiredTigerKVEngine* engine = checked_cast<WiredTigerKVEngine*>(
        opCtx->getServiceContext()->getStorageEngine()->getEngine());

    BSONObjBuilder bob;
    if (!WiredTigerUtil::collectConnectionStatistics(*engine, bob)) {
        LOGV2_DEBUG(7003148, 2, "WiredTiger is not ready to collect statistics.");
    }

    WiredTigerUtil::appendSnapshotWindowSettings(engine, &bob);

    {
        BSONObjBuilder subsection(bob.subobjStart("oplog"));
        subsection.append("visibility timestamp",
                          Timestamp(engine->getOplogManager()->getOplogReadTimestamp()));
    }

    {
        BSONObjBuilder subsection(bob.subobjStart("historyStorageStats"));
        if (!WiredTigerUtil::historyStoreStatistics(*engine, subsection)) {
            LOGV2_DEBUG(10100101, 2, "WiredTiger is not ready to collect statistics.");
        }
    }

    {
        BSONObjBuilder subsection(bob.subobjStart("connectionStats"));
        subsection.appendNumber("cached idle session count",
                                (long long)engine->getConnection().getIdleSessionsCount());
        subsection.appendNumber(
            "total engine time (ms)",
            (long long)durationCount<Milliseconds>(engine->getConnection().getTotalEngineTime()));
    }

    return bob.obj();
}

}  // namespace mongo
