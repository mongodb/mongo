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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(setAutoGetCollectionWait);

}  // namespace

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData dbName, LockMode mode, Date_t deadline)
    : _opCtx(opCtx), _dbName(dbName), _dbLock(opCtx, dbName, mode, deadline), _db([&] {
          auto databaseHolder = DatabaseHolder::get(opCtx);
          return databaseHolder->getDb(opCtx, dbName);
      }()) {
    auto dss = DatabaseShardingState::get(opCtx, dbName);
    auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
    dss->checkDbVersion(opCtx, dssLock);
}

Database* AutoGetDb::ensureDbExists() {
    if (_db) {
        return _db;
    }

    auto databaseHolder = DatabaseHolder::get(_opCtx);
    _db = databaseHolder->openDb(_opCtx, _dbName, nullptr);

    auto dss = DatabaseShardingState::get(_opCtx, _dbName);
    auto dssLock = DatabaseShardingState::DSSLock::lockShared(_opCtx, dss);
    dss->checkDbVersion(_opCtx, dssLock);

    return _db;
}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     ViewMode viewMode,
                                     Date_t deadline)
    : _autoDb(opCtx,
              !nsOrUUID.dbname().empty() ? nsOrUUID.dbname() : nsOrUUID.nss()->db(),
              isSharedLockMode(modeColl) ? MODE_IS : MODE_IX,
              deadline) {
    if (auto& nss = nsOrUUID.nss()) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace " << *nss << " is not a valid collection name",
                nss->isValid());
    }

    _collLock.emplace(opCtx, nsOrUUID, modeColl, deadline);
    _resolvedNss = CollectionCatalog::get(opCtx).resolveNamespaceStringOrUUID(opCtx, nsOrUUID);

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    setAutoGetCollectionWait.execute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    Database* const db = _autoDb.getDb();
    invariant(!nsOrUUID.uuid() || db,
              str::stream() << "Database for " << _resolvedNss.ns()
                            << " disappeared after successufully resolving "
                            << nsOrUUID.toString());

    // In most cases we expect modifications for system.views to upgrade MODE_IX to MODE_X before
    // taking the lock. One exception is a query by UUID of system.views in a transaction. Usual
    // queries of system.views (by name, not UUID) within a transaction are rejected. However, if
    // the query is by UUID we can't determine whether the namespace is actually system.views until
    // we take the lock here. So we have this one last assertion.
    uassert(51070,
            "Modifications to system.views must take an exclusive lock",
            !_resolvedNss.isSystemDotViews() || modeColl != MODE_IX);

    // If the database doesn't exists, we can't obtain a collection or check for views
    if (!db)
        return;

    _coll = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, _resolvedNss);
    invariant(!nsOrUUID.uuid() || _coll,
              str::stream() << "Collection for " << _resolvedNss.ns()
                            << " disappeared after successufully resolving "
                            << nsOrUUID.toString());

    if (_coll) {
        // If we are in a transaction, we cannot yield and wait when there are pending catalog
        // changes. Instead, we must return an error in such situations. We
        // ignore this restriction for the oplog, since it never has pending catalog changes.
        if (opCtx->inMultiDocumentTransaction() &&
            _resolvedNss != NamespaceString::kRsOplogNamespace) {

            if (auto minSnapshot = _coll->getMinimumVisibleSnapshot()) {
                auto mySnapshot = opCtx->recoveryUnit()->getPointInTimeReadTimestamp().get_value_or(
                    opCtx->recoveryUnit()->getCatalogConflictingTimestamp());

                uassert(ErrorCodes::SnapshotUnavailable,
                        str::stream()
                            << "Unable to read from a snapshot due to pending collection catalog "
                               "changes; please retry the operation. Snapshot timestamp is "
                            << mySnapshot.toString() << ". Collection minimum is "
                            << minSnapshot->toString(),
                        mySnapshot.isNull() || mySnapshot >= minSnapshot.get());
            }
        }

        // If the collection exists, there is no need to check for views.
        return;
    }

    _view = ViewCatalog::get(db)->lookup(opCtx, _resolvedNss.ns());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Namespace " << _resolvedNss.ns() << " is a view, not a collection",
            !_view || viewMode == kViewsPermitted);
}

AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* opCtx,
                                     StringData dbName,
                                     LockMode mode,
                                     Date_t deadline)
    : _autoDb(opCtx, dbName, mode, deadline), _db(_autoDb.ensureDbExists()) {
    invariant(mode == MODE_IX || mode == MODE_X);
}

ConcealCollectionCatalogChangesBlock::ConcealCollectionCatalogChangesBlock(OperationContext* opCtx)
    : _opCtx(opCtx) {
    CollectionCatalog::get(_opCtx).onCloseCatalog(_opCtx);
}

ConcealCollectionCatalogChangesBlock::~ConcealCollectionCatalogChangesBlock() {
    invariant(_opCtx);
    CollectionCatalog::get(_opCtx).onOpenCatalog(_opCtx);
}

ReadSourceScope::ReadSourceScope(OperationContext* opCtx,
                                 RecoveryUnit::ReadSource readSource,
                                 boost::optional<Timestamp> provided)
    : _opCtx(opCtx), _originalReadSource(opCtx->recoveryUnit()->getTimestampReadSource()) {

    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        _originalReadTimestamp = *_opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
    }

    _opCtx->recoveryUnit()->abandonSnapshot();
    _opCtx->recoveryUnit()->setTimestampReadSource(readSource, provided);
}

ReadSourceScope::~ReadSourceScope() {
    _opCtx->recoveryUnit()->abandonSnapshot();
    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        _opCtx->recoveryUnit()->setTimestampReadSource(_originalReadSource, _originalReadTimestamp);
    } else {
        _opCtx->recoveryUnit()->setTimestampReadSource(_originalReadSource);
    }
}

AutoGetOplog::AutoGetOplog(OperationContext* opCtx, OplogAccessMode mode, Date_t deadline)
    : _shouldNotConflictWithSecondaryBatchApplicationBlock(opCtx->lockState()) {
    auto lockMode = (mode == OplogAccessMode::kRead) ? MODE_IS : MODE_IX;
    if (mode == OplogAccessMode::kLogOp) {
        // Invariant that global lock is already held for kLogOp mode.
        invariant(opCtx->lockState()->isWriteLocked());
    } else {
        _globalLock.emplace(opCtx, lockMode, deadline, Lock::InterruptBehavior::kThrow);
    }

    // Obtain database and collection intent locks for non-document-locking storage engines.
    if (!opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {
        _dbWriteLock.emplace(opCtx, NamespaceString::kLocalDb, lockMode, deadline);
        _collWriteLock.emplace(opCtx, NamespaceString::kRsOplogNamespace, lockMode, deadline);
    }
    _oplogInfo = repl::LocalOplogInfo::get(opCtx);
    _oplog = _oplogInfo->getCollection();
}

}  // namespace mongo
