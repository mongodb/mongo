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

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/stats/top.h"
#include "mongo/s/d_state.h"

namespace mongo {

AutoGetDb::AutoGetDb(OperationContext* txn, StringData ns, LockMode mode)
    : _dbLock(txn->lockState(), ns, mode), _db(dbHolder().get(txn, ns)) {}

AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* txn, StringData ns, LockMode mode)
    : _transaction(txn, MODE_IX),
      _dbLock(txn->lockState(), ns, mode),
      _db(dbHolder().get(txn, ns)) {
    invariant(mode == MODE_IX || mode == MODE_X);
    _justCreated = false;
    // If the database didn't exist, relock in MODE_X
    if (_db == NULL) {
        if (mode != MODE_X) {
            _dbLock.relockWithMode(MODE_X);
        }
        _db = dbHolder().openDb(txn, ns);
        _justCreated = true;
    }
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* txn, const std::string& ns)
    : _txn(txn),
      _transaction(txn, MODE_IS),
      _db(_txn, nsToDatabaseSubstring(ns), MODE_IS),
      _collLock(_txn->lockState(), ns, MODE_IS),
      _coll(NULL) {
    _init(ns, nsToCollectionSubstring(ns));
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* txn,
                                                   const NamespaceString& nss)
    : _txn(txn),
      _transaction(txn, MODE_IS),
      _db(_txn, nss.db(), MODE_IS),
      _collLock(_txn->lockState(), nss.toString(), MODE_IS),
      _coll(NULL) {
    _init(nss.toString(), nss.coll());
}

void AutoGetCollectionForRead::_init(const std::string& ns, StringData coll) {
    massert(28535, "need a non-empty collection name", !coll.empty());

    // We have both the DB and collection locked, which the prerequisite to do a stable shard
    // version check.
    ensureShardVersionOKOrThrow(_txn->getClient(), ns);

    auto curOp = CurOp::get(_txn);
    stdx::lock_guard<Client> lk(*_txn->getClient());
    // TODO: OldClientContext legacy, needs to be removed
    curOp->ensureStarted();
    curOp->setNS_inlock(ns);

    // At this point, we are locked in shared mode for the database by the DB lock in the
    // constructor, so it is safe to load the DB pointer.
    if (_db.getDb()) {
        // TODO: OldClientContext legacy, needs to be removed
        curOp->enter_inlock(ns.c_str(), _db.getDb()->getProfilingLevel());

        _coll = _db.getDb()->getCollection(ns);
    }
}

AutoGetCollectionForRead::~AutoGetCollectionForRead() {
    // Report time spent in read lock
    auto currentOp = CurOp::get(_txn);
    Top::get(_txn->getClient()->getServiceContext())
        .record(currentOp->getNS(),
                currentOp->getOp(),
                -1,  // "read locked"
                _timer.micros(),
                currentOp->isCommand());
}


OldClientContext::OldClientContext(OperationContext* txn, const std::string& ns, Database* db)
    : _justCreated(false), _doVersion(true), _ns(ns), _db(db), _txn(txn) {}

OldClientContext::OldClientContext(OperationContext* txn,
                                   const std::string& ns,
                                   Database* db,
                                   bool justCreated)
    : _justCreated(justCreated), _doVersion(true), _ns(ns), _db(db), _txn(txn) {
    _finishInit();
}

OldClientContext::OldClientContext(OperationContext* txn,
                                   const std::string& ns,
                                   bool doVersion)
    : _justCreated(false),  // set for real in finishInit
      _doVersion(doVersion),
      _ns(ns),
      _db(NULL),
      _txn(txn) {
    _finishInit();
}

void OldClientContext::_finishInit() {
    _db = dbHolder().get(_txn, _ns);
    if (_db) {
        _justCreated = false;
    } else {
        invariant(_txn->lockState()->isDbLockedForMode(nsToDatabaseSubstring(_ns), MODE_X));
        _db = dbHolder().openDb(_txn, _ns, &_justCreated);
        invariant(_db);
    }

    if (_doVersion) {
        _checkNotStale();
    }

    stdx::lock_guard<Client> lk(*_txn->getClient());
    CurOp::get(_txn)->enter_inlock(_ns.c_str(), _db->getProfilingLevel());
}

void OldClientContext::_checkNotStale() const {
    switch (CurOp::get(_txn)->getOp()) {
        case dbGetMore:  // getMore is special and should be handled elsewhere.
        case dbUpdate:   // update & delete check shard version in instance.cpp, so don't check
        case dbDelete:   // here as well.
            break;
        default:
            ensureShardVersionOKOrThrow(_txn->getClient(), _ns);
    }
}

OldClientContext::~OldClientContext() {
    // Lock must still be held
    invariant(_txn->lockState()->isLocked());

    auto currentOp = CurOp::get(_txn);
    Top::get(_txn->getClient()->getServiceContext())
        .record(currentOp->getNS(),
                currentOp->getOp(),
                _txn->lockState()->isWriteLocked() ? 1 : -1,
                _timer.micros(),
                currentOp->isCommand());
}


OldClientWriteContext::OldClientWriteContext(OperationContext* opCtx, const std::string& ns)
    : _txn(opCtx),
      _nss(ns),
      _autodb(opCtx, _nss.db(), MODE_IX),
      _collk(opCtx->lockState(), ns, MODE_IX),
      _c(opCtx, ns, _autodb.getDb(), _autodb.justCreated()) {
    _collection = _c.db()->getCollection(ns);
    if (!_collection && !_autodb.justCreated()) {
        // relock database in MODE_X to allow collection creation
        _collk.relockAsDatabaseExclusive(_autodb.lock());
        Database* db = dbHolder().get(_txn, ns);
        invariant(db == _c.db());
    }
}

}  // namespace mongo
