/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/stats/top.h"

namespace mongo {

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData ns, LockMode mode)
    : _dbLock(opCtx, ns, mode), _db(dbHolder().get(opCtx, ns)) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     LockMode modeDB,
                                     LockMode modeColl,
                                     ViewMode viewMode)
    : _viewMode(viewMode),
      _autoDb(opCtx, nss.db(), modeDB),
      _collLock(opCtx->lockState(), nss.ns(), modeColl),
      _coll(_autoDb.getDb() ? _autoDb.getDb()->getCollection(nss) : nullptr) {
    Database* db = _autoDb.getDb();
    // If the database exists, but not the collection, check for views.
    if (_viewMode == ViewMode::kViewsForbidden && db && !_coll &&
        db->getViewCatalog()->lookup(opCtx, nss.ns()))
        uasserted(ErrorCodes::CommandNotSupportedOnView,
                  str::stream() << "Namespace " << nss.ns() << " is a view, not a collection");
}

AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* opCtx, StringData ns, LockMode mode)
    : _dbLock(opCtx, ns, mode), _db(dbHolder().get(opCtx, ns)) {
    invariant(mode == MODE_IX || mode == MODE_X);
    _justCreated = false;
    // If the database didn't exist, relock in MODE_X
    if (_db == NULL) {
        if (mode != MODE_X) {
            _dbLock.relockWithMode(MODE_X);
        }
        _db = dbHolder().openDb(opCtx, ns);
        _justCreated = true;
    }
}

AutoStatsTracker::AutoStatsTracker(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   Top::LockType lockType,
                                   boost::optional<int> dbProfilingLevel)
    : _opCtx(opCtx), _lockType(lockType) {
    if (!dbProfilingLevel) {
        // No profiling level was determined, attempt to read the profiling level from the Database
        // object.
        AutoGetDb autoDb(_opCtx, nss.db(), MODE_IS);
        if (autoDb.getDb()) {
            dbProfilingLevel = autoDb.getDb()->getProfilingLevel();
        }
    }
    stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
    CurOp::get(_opCtx)->enter_inlock(nss.ns().c_str(), dbProfilingLevel);
}

AutoStatsTracker::~AutoStatsTracker() {
    auto curOp = CurOp::get(_opCtx);
    Top::get(_opCtx->getServiceContext())
        .record(_opCtx,
                curOp->getNS(),
                curOp->getLogicalOp(),
                _lockType,
                _timer.micros(),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   AutoGetCollection::ViewMode viewMode) {
    _autoColl.emplace(opCtx, nss, MODE_IS, MODE_IS, viewMode);

    // Note: this can yield.
    _ensureMajorityCommittedSnapshotIsValid(nss, opCtx);
}

void AutoGetCollectionForRead::_ensureMajorityCommittedSnapshotIsValid(const NamespaceString& nss,
                                                                       OperationContext* opCtx) {
    while (true) {
        auto coll = _autoColl->getCollection();
        if (!coll) {
            return;
        }
        auto minSnapshot = coll->getMinimumVisibleSnapshot();
        if (!minSnapshot) {
            return;
        }
        auto mySnapshot = opCtx->recoveryUnit()->getMajorityCommittedSnapshot();
        if (!mySnapshot) {
            return;
        }
        if (mySnapshot >= minSnapshot) {
            return;
        }

        // Yield locks.
        _autoColl = boost::none;

        repl::ReplicationCoordinator::get(opCtx)->waitUntilSnapshotCommitted(opCtx, *minSnapshot);

        uassertStatusOK(opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->yielded();
        }

        // Relock.
        _autoColl.emplace(opCtx, nss, MODE_IS);
    }
}

AutoGetCollectionForReadCommand::AutoGetCollectionForReadCommand(
    OperationContext* opCtx, const NamespaceString& nss, AutoGetCollection::ViewMode viewMode) {

    _autoCollForRead.emplace(opCtx, nss, viewMode);
    const int doNotChangeProfilingLevel = 0;
    _statsTracker.emplace(opCtx,
                          nss,
                          Top::LockType::ReadLocked,
                          _autoCollForRead->getDb() ? _autoCollForRead->getDb()->getProfilingLevel()
                                                    : doNotChangeProfilingLevel);

    // We have both the DB and collection locked, which is the prerequisite to do a stable shard
    // version check, but we'd like to do the check after we have a satisfactory snapshot.
    auto css = CollectionShardingState::get(opCtx, nss);
    css->checkShardVersionOrThrow(opCtx);
}

AutoGetCollectionOrViewForReadCommand::AutoGetCollectionOrViewForReadCommand(
    OperationContext* opCtx, const NamespaceString& nss)
    : AutoGetCollectionForReadCommand(opCtx, nss, AutoGetCollection::ViewMode::kViewsPermitted),
      _view(_autoCollForRead->getDb() && !getCollection()
                ? _autoCollForRead->getDb()->getViewCatalog()->lookup(opCtx, nss.ns())
                : nullptr) {}

void AutoGetCollectionOrViewForReadCommand::releaseLocksForView() noexcept {
    invariant(_view);
    _view = nullptr;
    _autoCollForRead = boost::none;
}

OldClientContext::OldClientContext(OperationContext* opCtx,
                                   const std::string& ns,
                                   Database* db,
                                   bool justCreated)
    : _justCreated(justCreated), _doVersion(true), _ns(ns), _db(db), _opCtx(opCtx) {
    _finishInit();
}

OldClientContext::OldClientContext(OperationContext* opCtx,
                                   const std::string& ns,
                                   bool doVersion)
    : _justCreated(false),  // set for real in finishInit
      _doVersion(doVersion),
      _ns(ns),
      _db(NULL),
      _opCtx(opCtx) {
    _finishInit();
}

void OldClientContext::_finishInit() {
    _db = dbHolder().get(_opCtx, _ns);
    if (_db) {
        _justCreated = false;
    } else {
        invariant(_opCtx->lockState()->isDbLockedForMode(nsToDatabaseSubstring(_ns), MODE_X));
        _db = dbHolder().openDb(_opCtx, _ns, &_justCreated);
        invariant(_db);
    }

    if (_doVersion) {
        _checkNotStale();
    }

    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    CurOp::get(_opCtx)->enter_inlock(_ns.c_str(), _db->getProfilingLevel());
}

void OldClientContext::_checkNotStale() const {
    switch (CurOp::get(_opCtx)->getNetworkOp()) {
        case dbGetMore:  // getMore is special and should be handled elsewhere.
        case dbUpdate:   // update & delete check shard version in instance.cpp, so don't check
        case dbDelete:   // here as well.
            break;
        default:
            auto css = CollectionShardingState::get(_opCtx, _ns);
            css->checkShardVersionOrThrow(_opCtx);
    }
}

OldClientContext::~OldClientContext() {
    // Lock must still be held
    invariant(_opCtx->lockState()->isLocked());

    auto currentOp = CurOp::get(_opCtx);
    Top::get(_opCtx->getClient()->getServiceContext())
        .record(_opCtx,
                currentOp->getNS(),
                currentOp->getLogicalOp(),
                _opCtx->lockState()->isWriteLocked() ? Top::LockType::WriteLocked
                                                     : Top::LockType::ReadLocked,
                _timer.micros(),
                currentOp->isCommand(),
                currentOp->getReadWriteType());
}


OldClientWriteContext::OldClientWriteContext(OperationContext* opCtx, const std::string& ns)
    : _opCtx(opCtx),
      _nss(ns),
      _autodb(opCtx, _nss.db(), MODE_IX),
      _collk(opCtx->lockState(), ns, MODE_IX),
      _c(opCtx, ns, _autodb.getDb(), _autodb.justCreated()) {
    _collection = _c.db()->getCollection(ns);
    if (!_collection && !_autodb.justCreated()) {
        // relock database in MODE_X to allow collection creation
        _collk.relockAsDatabaseExclusive(_autodb.lock());
        Database* db = dbHolder().get(_opCtx, ns);
        invariant(db == _c.db());
    }
}

}  // namespace mongo
