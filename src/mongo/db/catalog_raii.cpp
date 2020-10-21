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
                                     AutoGetCollectionViewMode viewMode,
                                     Date_t deadline)
    : _opCtx(opCtx),
      _autoDb(opCtx,
              !nsOrUUID.dbname().empty() ? nsOrUUID.dbname() : nsOrUUID.nss()->db(),
              isSharedLockMode(modeColl) ? MODE_IS : MODE_IX,
              deadline) {
    auto& nss = nsOrUUID.nss();
    if (nss) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace " << *nss << " is not a valid collection name",
                nss->isValid());
    }

    // Out of an abundance of caution, force operations to acquire new snapshots after
    // acquiring exclusive collection locks. Operations that hold MODE_X locks make an
    // assumption that all writes are visible in their snapshot and no new writes will commit.
    // This may not be the case if an operation already has a snapshot open before acquiring an
    // exclusive lock.
    if (modeColl == MODE_X) {
        invariant(!opCtx->recoveryUnit()->isActive(),
                  str::stream() << "Snapshot opened before acquiring X lock for " << nsOrUUID);
    }

    _collLock.emplace(opCtx, nsOrUUID, modeColl, deadline);
    _resolvedNss = CollectionCatalog::get(opCtx).resolveNamespaceStringOrUUID(opCtx, nsOrUUID);

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    setAutoGetCollectionWait.execute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    Database* const db = _autoDb.getDb();
    invariant(!nsOrUUID.uuid() || db,
              str::stream() << "Database for " << _resolvedNss.ns()
                            << " disappeared after successfully resolving " << nsOrUUID.toString());

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
            !_view || viewMode == AutoGetCollectionViewMode::kViewsPermitted);
}

Collection* AutoGetCollection::getWritableCollection(CollectionCatalog::LifetimeMode mode) {
    // Acquire writable instance if not already available
    if (!_writableColl) {

        // Makes the internal CollectionPtr Yieldable and resets the writable Collection when the
        // write unit of work finishes so we re-fetches and re-clones the Collection if a new write
        // unit of work is opened.
        class WritableCollectionReset : public RecoveryUnit::Change {
        public:
            WritableCollectionReset(AutoGetCollection& autoColl,
                                    const Collection* originalCollection)
                : _autoColl(autoColl), _originalCollection(originalCollection) {}
            void commit(boost::optional<Timestamp> commitTime) final {
                _autoColl._coll = CollectionPtr(_autoColl.getOperationContext(),
                                                _autoColl._coll.get(),
                                                LookupCollectionForYieldRestore());
                _autoColl._writableColl = nullptr;
            }
            void rollback() final {
                _autoColl._coll = CollectionPtr(_autoColl.getOperationContext(),
                                                _originalCollection,
                                                LookupCollectionForYieldRestore());
                _autoColl._writableColl = nullptr;
            }

        private:
            AutoGetCollection& _autoColl;
            // Used to be able to restore to the original pointer in case of a rollback
            const Collection* _originalCollection;
        };

        auto& catalog = CollectionCatalog::get(_opCtx);
        _writableColl =
            catalog.lookupCollectionByNamespaceForMetadataWrite(_opCtx, mode, _resolvedNss);
        if (mode == CollectionCatalog::LifetimeMode::kManagedInWriteUnitOfWork) {
            _opCtx->recoveryUnit()->registerChange(
                std::make_unique<WritableCollectionReset>(*this, _coll.get()));
        }

        // Set to writable collection. We are no longer yieldable.
        _coll = _writableColl;
    }
    return _writableColl;
}

AutoGetCollectionLockFree::AutoGetCollectionLockFree(OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     AutoGetCollectionViewMode viewMode,
                                                     Date_t deadline)
    : _globalLock(
          opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, true /* skipRSTLLock */) {

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    setAutoGetCollectionWait.execute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    _resolvedNss = CollectionCatalog::get(opCtx).resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
    _collection =
        CollectionCatalog::get(opCtx).lookupCollectionByNamespaceForRead(opCtx, _resolvedNss);

    // When we restore from yield on this CollectionPtr we will update _collection above and use its
    // new pointer in the CollectionPtr
    _collectionPtr = CollectionPtr(
        opCtx, _collection.get(), [this](OperationContext* opCtx, CollectionUUID uuid) {
            _collection = CollectionCatalog::get(opCtx).lookupCollectionByUUIDForRead(opCtx, uuid);
            return _collection.get();
        });

    // TODO (SERVER-51319): add DatabaseShardingState::checkDbVersion somewhere.

    if (_collection) {
        // If the collection exists, there is no need to check for views.
        return;
    }

    // Returns nullptr for 'viewCatalog' if db does not exist.
    auto viewCatalog = DatabaseHolder::get(opCtx)->getSharedViewCatalog(opCtx, _resolvedNss.db());
    if (!viewCatalog) {
        return;
    }

    // TODO (SERVER-51320): this code will invariant in ViewCatalog::lookup() because it takes a
    // CollectionLock that expects a DBLock to be held on the database already.
    _view = viewCatalog->lookup(opCtx, _resolvedNss.ns());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Namespace " << _resolvedNss.ns() << " is a view, not a collection",
            !_view || viewMode == AutoGetCollectionViewMode::kViewsPermitted);
}

struct CollectionWriter::SharedImpl {
    SharedImpl(CollectionWriter* parent) : _parent(parent) {}

    CollectionWriter* _parent;
    std::function<Collection*(CollectionCatalog::LifetimeMode)> _writableCollectionInitializer;
};

CollectionWriter::CollectionWriter(OperationContext* opCtx,
                                   const CollectionUUID& uuid,
                                   CollectionCatalog::LifetimeMode mode)
    : _collection(&_storedCollection),
      _opCtx(opCtx),
      _mode(mode),
      _sharedImpl(std::make_shared<SharedImpl>(this)) {

    _storedCollection = CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, uuid);
    _sharedImpl->_writableCollectionInitializer = [opCtx,
                                                   uuid](CollectionCatalog::LifetimeMode mode) {
        return CollectionCatalog::get(opCtx).lookupCollectionByUUIDForMetadataWrite(
            opCtx, mode, uuid);
    };
}

CollectionWriter::CollectionWriter(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   CollectionCatalog::LifetimeMode mode)
    : _collection(&_storedCollection),
      _opCtx(opCtx),
      _mode(mode),
      _sharedImpl(std::make_shared<SharedImpl>(this)) {
    _storedCollection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
    _sharedImpl->_writableCollectionInitializer = [opCtx,
                                                   nss](CollectionCatalog::LifetimeMode mode) {
        return CollectionCatalog::get(opCtx).lookupCollectionByNamespaceForMetadataWrite(
            opCtx, mode, nss);
    };
}

CollectionWriter::CollectionWriter(AutoGetCollection& autoCollection,
                                   CollectionCatalog::LifetimeMode mode)
    : _collection(&autoCollection.getCollection()),
      _opCtx(autoCollection.getOperationContext()),
      _mode(mode),
      _sharedImpl(std::make_shared<SharedImpl>(this)) {
    _sharedImpl->_writableCollectionInitializer =
        [&autoCollection](CollectionCatalog::LifetimeMode mode) {
            return autoCollection.getWritableCollection(mode);
        };
}

CollectionWriter::CollectionWriter(Collection* writableCollection)
    : _collection(&_storedCollection),
      _storedCollection(writableCollection),
      _writableCollection(writableCollection),
      _mode(CollectionCatalog::LifetimeMode::kInplace) {}

CollectionWriter::~CollectionWriter() {
    // Notify shared state that this instance is destroyed
    if (_sharedImpl) {
        _sharedImpl->_parent = nullptr;
    }

    if (_mode == CollectionCatalog::LifetimeMode::kUnmanagedClone && _writableCollection) {
        CollectionCatalog::get(_opCtx).discardUnmanagedClone(_opCtx, _writableCollection);
    }
}

Collection* CollectionWriter::getWritableCollection() {
    // Acquire writable instance lazily if not already available
    if (!_writableCollection) {
        _writableCollection = _sharedImpl->_writableCollectionInitializer(_mode);

        // Resets the writable Collection when the write unit of work finishes so we re-fetch and
        // re-clone the Collection if a new write unit of work is opened. Holds the back pointer to
        // the CollectionWriter via a shared_ptr so we can detect if the instance is already
        // destroyed.
        class WritableCollectionReset : public RecoveryUnit::Change {
        public:
            WritableCollectionReset(std::shared_ptr<SharedImpl> shared,
                                    CollectionPtr rollbackCollection)
                : _shared(std::move(shared)), _rollbackCollection(std::move(rollbackCollection)) {}
            void commit(boost::optional<Timestamp> commitTime) final {
                if (_shared->_parent)
                    _shared->_parent->_writableCollection = nullptr;
            }
            void rollback() final {
                if (_shared->_parent) {
                    _shared->_parent->_storedCollection = std::move(_rollbackCollection);
                    _shared->_parent->_writableCollection = nullptr;
                }
            }

        private:
            std::shared_ptr<SharedImpl> _shared;
            CollectionPtr _rollbackCollection;
        };

        // If we are using our stored Collection then we are not managed by an AutoGetCollection and
        // we need to manage lifetime here.
        if (_mode == CollectionCatalog::LifetimeMode::kManagedInWriteUnitOfWork) {
            bool usingStoredCollection = *_collection == _storedCollection;
            _opCtx->recoveryUnit()->registerChange(std::make_unique<WritableCollectionReset>(
                _sharedImpl,
                usingStoredCollection ? std::move(_storedCollection) : CollectionPtr()));
            if (usingStoredCollection) {
                _storedCollection = _writableCollection;
            }
        }
    }
    return _writableCollection;
}

void CollectionWriter::commitToCatalog() {
    dassert(_mode == CollectionCatalog::LifetimeMode::kUnmanagedClone);
    dassert(_writableCollection);
    CollectionCatalog::get(_opCtx).commitUnmanagedClone(_opCtx, _writableCollection);
    _writableCollection = nullptr;
}

LockMode fixLockModeForSystemDotViewsChanges(const NamespaceString& nss, LockMode mode) {
    return nss.isSystemDotViews() ? MODE_X : mode;
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

    _oplogInfo = repl::LocalOplogInfo::get(opCtx);
    _oplog = &_oplogInfo->getCollection();
}

}  // namespace mongo
