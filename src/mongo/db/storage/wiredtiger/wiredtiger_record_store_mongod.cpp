/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
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

class WiredTigerRecordStoreThread : public BackgroundJob {
public:
    WiredTigerRecordStoreThread(const NamespaceString& ns)
        : BackgroundJob(true /* deleteSelf */), _ns(ns) {
        _name = std::string("WT RecordStoreThread: ") + _ns.toString();
    }

    virtual std::string name() const {
        return _name;
    }

    /**
     * Returns true iff there was an oplog to delete from.
     */
    bool _deleteExcessDocuments() {
        if (!getGlobalServiceContext()->getGlobalStorageEngine()) {
            LOG(2) << "no global storage engine yet";
            return false;
        }

        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;

        try {
            ScopedTransaction transaction(&txn, MODE_IX);

            AutoGetDb autoDb(&txn, _ns.db(), MODE_IX);
            Database* db = autoDb.getDb();
            if (!db) {
                LOG(2) << "no local database yet";
                return false;
            }

            Lock::CollectionLock collectionLock(txn.lockState(), _ns.ns(), MODE_IX);
            Collection* collection = db->getCollection(_ns);
            if (!collection) {
                LOG(2) << "no collection " << _ns;
                return false;
            }

            OldClientContext ctx(&txn, _ns.ns(), false);
            WiredTigerRecordStore* rs =
                checked_cast<WiredTigerRecordStore*>(collection->getRecordStore());

            if (!rs->yieldAndAwaitOplogDeletionRequest(&txn)) {
                return false;  // Oplog went away.
            }
            rs->reclaimOplog(&txn);
        } catch (const std::exception& e) {
            severe() << "error in WiredTigerRecordStoreThread: " << e.what();
            fassertFailedNoTrace(!"error in WiredTigerRecordStoreThread");
        } catch (...) {
            fassertFailedNoTrace(!"unknown error in WiredTigerRecordStoreThread");
        }
        return true;
    }

    virtual void run() {
        Client::initThread(_name.c_str());

        while (!inShutdown()) {
            if (!_deleteExcessDocuments()) {
                sleepmillis(1000);  // Back off in case there were problems deleting.
            }
        }
    }

private:
    NamespaceString _ns;
    std::string _name;
};

}  // namespace

// static
bool WiredTigerKVEngine::initRsOplogBackgroundThread(StringData ns) {
    if (!NamespaceString::oplog(ns)) {
        return false;
    }

    if (storageGlobalParams.repair) {
        LOG(1) << "not starting WiredTigerRecordStoreThread for " << ns
               << " because we are in repair";
        return false;
    }

    stdx::lock_guard<stdx::mutex> lock(_backgroundThreadMutex);
    NamespaceString nss(ns);
    if (_backgroundThreadNamespaces.count(nss)) {
        log() << "WiredTigerRecordStoreThread " << ns << " already started";
    } else {
        log() << "Starting WiredTigerRecordStoreThread " << ns;
        BackgroundJob* backgroundThread = new WiredTigerRecordStoreThread(nss);
        backgroundThread->go();
        _backgroundThreadNamespaces.insert(nss);
    }
    return true;
}

}  // namespace mongo
