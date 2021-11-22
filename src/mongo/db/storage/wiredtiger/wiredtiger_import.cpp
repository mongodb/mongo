/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"

#include <wiredtiger.h>

#include "mongo/base/status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"

namespace mongo {
void wiredTigerImportFromBackupCursor(OperationContext* opCtx, std::string importPath) {
    WT_CONNECTION* conn;
    // Accept WT's default checkpoint behavior: take a checkpoint only when opening and closing.
    // WT converts the imported WiredTiger.backup file to a fresh WiredTiger.wt file, rolls back to
    // stable, and takes a checkpoint.
    auto status = wtRCToStatus(
        wiredtiger_open(importPath.c_str(),
                        nullptr,
                        "config_base=false,log=(enabled=true,path=journal,compressor=snappy)",
                        &conn));

    if (status.isOK()) {
        LOGV2_DEBUG(6113700, 1, "Opened donor WiredTiger database");
    } else {
        LOGV2_ERROR(6113701, "Failed to open donor WiredTiger database", "status"_attr = status);
    }

    // TODO (SERVER-61138): Record collections' metadata.
    conn->close(conn, nullptr);
    // TODO (SERVER-61143): Import collections into main WT instance.
}
}  // namespace mongo
