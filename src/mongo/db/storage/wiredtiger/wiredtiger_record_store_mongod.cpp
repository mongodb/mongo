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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/background.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

std::set<NamespaceString> _backgroundThreadNamespaces;
stdx::mutex _backgroundThreadMutex;

class OplogTruncaterThread : public BackgroundJob {
public:
    OplogTruncaterThread(const NamespaceString& ns)
        : BackgroundJob(true /* deleteSelf */), _ns(ns) {
        _name = std::string("WT-OplogTruncaterThread-") + _ns.toString();
    }

    virtual std::string name() const {
        return _name;
    }

    /**
     * Returns true iff there was an oplog to delete from.
     */
    bool _deleteExcessDocuments() {
        if (!getGlobalServiceContext()->getStorageEngine()) {
            LOG(2) << "no global storage engine yet";
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

            WiredTigerRecordStore* rs = nullptr;
            {
                // Release the database lock right away because we don't want to
                // block other operations on the local database and given the
                // fact that oplog collection is so special, Global IX lock can
                // make sure the collection exists.
                Lock::DBLock dbLock(opCtx.get(), _ns.db(), MODE_IX);
                auto databaseHolder = DatabaseHolder::get(opCtx.get());
                auto db = databaseHolder->getDb(opCtx.get(), _ns.db());
                if (!db) {
                    LOG(2) << "no local database yet";
                    return false;
                }
                // We need to hold the database lock while getting the collection. Otherwise a
                // concurrent collection creation would write to the map in the Database object
                // while we concurrently read the map.
                Collection* collection = db->getCollection(opCtx.get(), _ns);
                if (!collection) {
                    LOG(2) << "no collection " << _ns;
                    return false;
                }
                rs = checked_cast<WiredTigerRecordStore*>(collection->getRecordStore());
            }

            if (!rs->yieldAndAwaitOplogDeletionRequest(opCtx.get())) {
                return false;  // Oplog went away.
            }
            rs->reclaimOplog(opCtx.get());
        } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
            return false;
        } catch (const std::exception& e) {
            severe() << "error in OplogTruncaterThread: " << e.what();
            fassertFailedNoTrace(!"error in OplogTruncaterThread");
        } catch (...) {
            fassertFailedNoTrace(!"unknown error in OplogTruncaterThread");
        }
        return true;
    }

    virtual void run() {
        ThreadClient tc(_name, getGlobalServiceContext());

        while (!globalInShutdownDeprecated()) {
            if (!_deleteExcessDocuments()) {
                sleepmillis(1000);  // Back off in case there were problems deleting.
            }
        }
    }

private:
    NamespaceString _ns;
    std::string _name;
};

bool initRsOplogBackgroundThread(StringData ns) {
    if (!NamespaceString::oplog(ns)) {
        return false;
    }

    if (storageGlobalParams.repair || storageGlobalParams.readOnly) {
        LOG(1) << "not starting OplogTruncaterThread for " << ns
               << " because we are either in repair or read-only mode";
        return false;
    }

    stdx::lock_guard<stdx::mutex> lock(_backgroundThreadMutex);
    NamespaceString nss(ns);
    if (_backgroundThreadNamespaces.count(nss)) {
        log() << "OplogTruncaterThread " << ns << " already started";
    } else {
        log() << "Starting OplogTruncaterThread " << ns;
        BackgroundJob* backgroundThread = new OplogTruncaterThread(nss);
        backgroundThread->go();
        _backgroundThreadNamespaces.insert(nss);
    }
    return true;
}

MONGO_INITIALIZER(SetInitRsOplogBackgroundThreadCallback)(InitializerContext* context) {
    WiredTigerKVEngine::setInitRsOplogBackgroundThreadCallback(initRsOplogBackgroundThread);
    return Status::OK();
}

}  // namespace
}  // namespace mongo
