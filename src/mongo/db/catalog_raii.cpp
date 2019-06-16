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
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(setAutoGetCollectionWait);

}  // namespace

AutoGetDb::AutoGetDb(OperationContext* opCtx, StringData dbName, LockMode mode, Date_t deadline)
    : _dbLock(opCtx, dbName, mode, deadline), _db([&] {
          auto databaseHolder = DatabaseHolder::get(opCtx);
          return databaseHolder->getDb(opCtx, dbName);
      }()) {
    if (_db) {
        auto& dss = DatabaseShardingState::get(_db);
        auto dssLock = DatabaseShardingState::DSSLock::lock(opCtx, &dss);
        dss.checkDbVersion(opCtx, dssLock);
    }
}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     ViewMode viewMode,
                                     Date_t deadline)
    : _autoDb(opCtx,
              !nsOrUUID.dbname().empty() ? nsOrUUID.dbname() : nsOrUUID.nss()->db(),
              isSharedLockMode(modeColl) ? MODE_IS : MODE_IX,
              deadline),
      _resolvedNss(resolveNamespaceStringOrUUID(opCtx, nsOrUUID)) {

    NamespaceString prevResolvedNss;
    do {
        _collLock.emplace(opCtx, _resolvedNss, modeColl, deadline);

        // We looked up nsOrUUID without a collection lock so it's possible that the
        // collection is dropped now. Look it up again.
        prevResolvedNss = _resolvedNss;
        _resolvedNss = resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
    } while (_resolvedNss != prevResolvedNss);

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    MONGO_FAIL_POINT_BLOCK(setAutoGetCollectionWait, customWait) {
        const BSONObj& data = customWait.getData();
        sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
    }

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

    _coll = db->getCollection(opCtx, _resolvedNss);
    invariant(!nsOrUUID.uuid() || _coll,
              str::stream() << "Collection for " << _resolvedNss.ns()
                            << " disappeared after successufully resolving "
                            << nsOrUUID.toString());

    if (_coll) {
        // Unlike read concern majority, read concern snapshot cannot yield and wait when there are
        // pending catalog changes. Instead, we must return an error in such situations. We ignore
        // this restriction for the oplog, since it never has pending catalog changes.
        auto readConcernLevel = repl::ReadConcernArgs::get(opCtx).getLevel();
        if (readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern &&
            _resolvedNss != NamespaceString::kRsOplogNamespace) {
            auto mySnapshot = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
            if (mySnapshot) {
                auto minSnapshot = _coll->getMinimumVisibleSnapshot();
                uassert(ErrorCodes::SnapshotUnavailable,
                        str::stream()
                            << "Unable to read from a snapshot due to pending collection catalog "
                               "changes; please retry the operation. Snapshot timestamp is "
                            << mySnapshot->toString()
                            << ". Collection minimum is "
                            << minSnapshot->toString(),
                        !minSnapshot || *mySnapshot >= *minSnapshot);
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

NamespaceString AutoGetCollection::resolveNamespaceStringOrUUID(OperationContext* opCtx,
                                                                NamespaceStringOrUUID nsOrUUID) {
    if (auto& nss = nsOrUUID.nss()) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace " << *nss << " is not a valid collection name",
                nss->isValid());
        return *nss;
    }

    CollectionCatalog& catalog = CollectionCatalog::get(opCtx);
    auto resolvedNss = catalog.lookupNSSByUUID(*nsOrUUID.uuid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Unable to resolve " << nsOrUUID.toString(),
            resolvedNss && resolvedNss->isValid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "UUID " << nsOrUUID.toString() << " specified in " << nsOrUUID.dbname()
                          << " resolved to a collection in a different database: "
                          << *resolvedNss,
            resolvedNss->db() == nsOrUUID.dbname());

    return *resolvedNss;
}

AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* opCtx,
                                     StringData dbName,
                                     LockMode mode,
                                     Date_t deadline) {
    invariant(mode == MODE_IX || mode == MODE_X);

    _autoDb.emplace(opCtx, dbName, mode, deadline);
    _db = _autoDb->getDb();

    // If the database didn't exist, relock in MODE_X
    if (!_db) {
        if (mode != MODE_X) {
            _autoDb.emplace(opCtx, dbName, MODE_X, deadline);
        }

        auto databaseHolder = DatabaseHolder::get(opCtx);
        _db = databaseHolder->openDb(opCtx, dbName, &_justCreated);
    }

    auto& dss = DatabaseShardingState::get(_db);
    auto dssLock = DatabaseShardingState::DSSLock::lock(opCtx, &dss);
    dss.checkDbVersion(opCtx, dssLock);
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

}  // namespace mongo
