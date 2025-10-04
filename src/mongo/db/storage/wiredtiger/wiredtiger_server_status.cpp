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

#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
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
    }

    return bob.obj();
}

}  // namespace mongo
