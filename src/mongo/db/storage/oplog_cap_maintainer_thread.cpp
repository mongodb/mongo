/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "oplog_cap_maintainer_thread.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/time_support.h"

namespace mongo {

bool OplogCapMaintainerThread::_deleteExcessDocuments() {
    if (!getGlobalServiceContext()->getStorageEngine()) {
        LOGV2_DEBUG(22240, 2, "OplogCapMaintainerThread: no global storage engine yet");
        return false;
    }

    const ServiceContext::UniqueOperationContext opCtx = cc().makeOperationContext();

    // Non-replicated writes will not contribute to replication lag and can be safely excluded from
    // Flow Control.
    opCtx->setShouldParticipateInFlowControl(false);

    try {
        // A Global IX lock should be good enough to protect the oplog truncation from
        // interruptions such as restartCatalog. PBWM, database lock or collection lock is not
        // needed. This improves concurrency if oplog truncation takes long time.
        AutoGetOplog oplogWrite(opCtx.get(), OplogAccessMode::kWrite);
        auto oplog = oplogWrite.getCollection();
        if (!oplog) {
            LOGV2_DEBUG(4562600, 2, "oplog collection does not exist");
            return false;
        }
        auto rs = oplog->getRecordStore();
        if (!rs->yieldAndAwaitOplogDeletionRequest(opCtx.get())) {
            return false;  // Oplog went away.
        }
        rs->reclaimOplog(opCtx.get());
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        return false;
    } catch (const std::exception& e) {
        LOGV2_FATAL_NOTRACE(
            22243, "error in OplogCapMaintainerThread: {e_what}", "e_what"_attr = e.what());
    } catch (...) {
        fassertFailedNoTrace(!"unknown error in OplogCapMaintainerThread");
    }
    return true;
}

void OplogCapMaintainerThread::run() {
    ThreadClient tc(_name, getGlobalServiceContext());

    while (!globalInShutdownDeprecated()) {
        if (!_deleteExcessDocuments()) {
            sleepmillis(1000);  // Back off in case there were problems deleting.
        }
    }
}
}  // namespace mongo
