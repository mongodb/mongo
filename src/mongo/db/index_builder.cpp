/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index_builder.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::endl;

AtomicUInt32 IndexBuilder::_indexBuildCount;

namespace {
// Synchronization tools when replication spawns a background index in a new thread.
// The bool is 'true' when a new background index has started in a new thread but the
// parent thread has not yet synchronized with it.
bool _bgIndexStarting(false);
stdx::mutex _bgIndexStartingMutex;
stdx::condition_variable _bgIndexStartingCondVar;

void _setBgIndexStarting() {
    stdx::lock_guard<stdx::mutex> lk(_bgIndexStartingMutex);
    invariant(_bgIndexStarting == false);
    _bgIndexStarting = true;
    _bgIndexStartingCondVar.notify_one();
}
}  // namespace

IndexBuilder::IndexBuilder(const BSONObj& index)
    : BackgroundJob(true /* self-delete */),
      _index(index.getOwned()),
      _name(str::stream() << "repl index builder " << _indexBuildCount.addAndFetch(1)) {}

IndexBuilder::~IndexBuilder() {}

std::string IndexBuilder::name() const {
    return _name;
}

void IndexBuilder::run() {
    Client::initThread(name().c_str());
    LOG(2) << "IndexBuilder building index " << _index;

    const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
    OperationContext& txn = *txnPtr;
    txn.lockState()->setIsBatchWriter(true);

    AuthorizationSession::get(txn.getClient())->grantInternalAuthorization();

    {
        stdx::lock_guard<Client> lk(*txn.getClient());
        CurOp::get(txn)->setNetworkOp_inlock(dbInsert);
    }
    NamespaceString ns(_index["ns"].String());

    ScopedTransaction transaction(&txn, MODE_IX);
    Lock::DBLock dlk(txn.lockState(), ns.db(), MODE_X);
    OldClientContext ctx(&txn, ns.getSystemIndexesCollection());

    Database* db = dbHolder().get(&txn, ns.db().toString());

    Status status = _build(&txn, db, true, &dlk);
    if (!status.isOK()) {
        error() << "IndexBuilder could not build index: " << status.toString();
        fassert(28555, ErrorCodes::isInterruption(status.code()));
    }
}

Status IndexBuilder::buildInForeground(OperationContext* txn, Database* db) const {
    return _build(txn, db, false, NULL);
}

void IndexBuilder::waitForBgIndexStarting() {
    stdx::unique_lock<stdx::mutex> lk(_bgIndexStartingMutex);
    while (_bgIndexStarting == false) {
        _bgIndexStartingCondVar.wait(lk);
    }
    // Reset for next time.
    _bgIndexStarting = false;
}

Status IndexBuilder::_build(OperationContext* txn,
                            Database* db,
                            bool allowBackgroundBuilding,
                            Lock::DBLock* dbLock) const {
    const NamespaceString ns(_index["ns"].String());

    Collection* c = db->getCollection(ns.ns());
    if (!c) {
        while (true) {
            try {
                WriteUnitOfWork wunit(txn);
                c = db->getOrCreateCollection(txn, ns.ns());
                verify(c);
                wunit.commit();
                break;
            } catch (const WriteConflictException& wce) {
                LOG(2) << "WriteConflictException while creating collection in IndexBuilder"
                       << ", retrying.";
                txn->recoveryUnit()->abandonSnapshot();
                continue;
            }
        }
    }

    {
        stdx::lock_guard<Client> lk(*txn->getClient());
        // Show which index we're building in the curop display.
        CurOp::get(txn)->setQuery_inlock(_index);
    }

    bool haveSetBgIndexStarting = false;
    while (true) {
        Status status = Status::OK();
        try {
            MultiIndexBlock indexer(txn, c);
            indexer.allowInterruption();

            if (allowBackgroundBuilding)
                indexer.allowBackgroundBuilding();


            try {
                status = indexer.init(_index);
                if (status.code() == ErrorCodes::IndexAlreadyExists) {
                    if (allowBackgroundBuilding) {
                        // Must set this in case anyone is waiting for this build.
                        _setBgIndexStarting();
                    }
                    return Status::OK();
                }

                if (status.isOK()) {
                    if (allowBackgroundBuilding) {
                        if (!haveSetBgIndexStarting) {
                            _setBgIndexStarting();
                            haveSetBgIndexStarting = true;
                        }
                        invariant(dbLock);
                        dbLock->relockWithMode(MODE_IX);
                    }

                    Lock::CollectionLock colLock(txn->lockState(), ns.ns(), MODE_IX);
                    status = indexer.insertAllDocumentsInCollection();
                }

                if (status.isOK()) {
                    if (allowBackgroundBuilding) {
                        dbLock->relockWithMode(MODE_X);
                    }
                    WriteUnitOfWork wunit(txn);
                    indexer.commit();
                    wunit.commit();
                }
                if (!status.isOK()) {
                    error() << "bad status from index build: " << status;
                }
            } catch (const DBException& e) {
                status = e.toStatus();
            }

            if (allowBackgroundBuilding) {
                dbLock->relockWithMode(MODE_X);
                Database* reloadDb = dbHolder().get(txn, ns.db());
                fassert(28553, reloadDb);
                fassert(28554, reloadDb->getCollection(ns.ns()));
            }

            if (status.code() == ErrorCodes::InterruptedAtShutdown) {
                // leave it as-if kill -9 happened. This will be handled on restart.
                indexer.abortWithoutCleanup();
            }
        } catch (const WriteConflictException& wce) {
            status = wce.toStatus();
        }

        if (status.code() != ErrorCodes::WriteConflict)
            return status;


        LOG(2) << "WriteConflictException while creating index in IndexBuilder, retrying.";
        txn->recoveryUnit()->abandonSnapshot();
    }
}
}
