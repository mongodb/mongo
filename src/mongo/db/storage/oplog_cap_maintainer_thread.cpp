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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {

bool OplogCapMaintainerThread::_deleteExcessDocuments() {
    if (!getGlobalServiceContext()->getStorageEngine()) {
        LOG(2) << "OplogCapMaintainerThread: no global storage engine yet";
        return false;
    }

    const ServiceContext::UniqueOperationContext opCtx = cc().makeOperationContext();

    try {
        // A Global IX lock should be good enough to protect the oplog truncation from
        // interruptions such as restartCatalog. PBWM, database lock or collection lock is not
        // needed. This improves concurrency if oplog truncation takes long time.
        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx.get()->lockState());
        Lock::GlobalLock lk(opCtx.get(), MODE_IX);

        RecordStore* rs = nullptr;
        NamespaceString oplogNss = NamespaceString::kRsOplogNamespace;
        {
            // Release the database lock right away because we don't want to
            // block other operations on the local database and given the
            // fact that oplog collection is so special, Global IX lock can
            // make sure the collection exists.
            Lock::DBLock dbLock(opCtx.get(), oplogNss.db(), MODE_IX);
            auto databaseHolder = DatabaseHolder::get(opCtx.get());
            auto db = databaseHolder->getDb(opCtx.get(), oplogNss.db());
            if (!db) {
                LOG(2) << "no local database yet";
                return false;
            }
            // We need to hold the database lock while getting the collection. Otherwise a
            // concurrent collection creation would write to the map in the Database object
            // while we concurrently read the map.
            Collection* collection = db->getCollection(opCtx.get(), oplogNss);
            if (!collection) {
                LOG(2) << "no collection " << oplogNss;
                return false;
            }
            rs = collection->getRecordStore();
        }
        if (!rs->yieldAndAwaitOplogDeletionRequest(opCtx.get())) {
            return false;  // Oplog went away.
        }
        rs->reclaimOplog(opCtx.get());
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        return false;
    } catch (const std::exception& e) {
        severe() << "error in OplogCapMaintainerThread: " << e.what();
        fassertFailedNoTrace(!"error in OplogCapMaintainerThread");
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
