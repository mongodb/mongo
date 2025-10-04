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

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/local_oplog_info.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/views/view.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CollectionCatalog;

namespace auto_get_collection {
enum class ViewMode { kViewsPermitted, kViewsForbidden };

struct Options {
    Options viewMode(ViewMode viewMode) {
        _viewMode = viewMode;
        return std::move(*static_cast<Options*>(this));
    }

    Options deadline(Date_t deadline) {
        _deadline = std::move(deadline);
        return std::move(*static_cast<Options*>(this));
    }

    Options expectedUUID(boost::optional<UUID> expectedUUID) {
        _expectedUUID = expectedUUID;
        return std::move(*static_cast<Options*>(this));
    }

    Options globalLockOptions(boost::optional<Lock::GlobalLockOptions> globalLockOptions) {
        _globalLockOptions = globalLockOptions;
        return std::move(*static_cast<Options*>(this));
    }

    ViewMode _viewMode = ViewMode::kViewsForbidden;
    Date_t _deadline = Date_t::max();
    boost::optional<UUID> _expectedUUID;
    boost::optional<Lock::GlobalLockOptions> _globalLockOptions;
};

}  // namespace auto_get_collection

/**
 * RAII-style class, which acquires a lock on the specified database in the requested mode and
 * obtains a reference to the database. Used as a shortcut for calls to
 * DatabaseHolder::get(opCtx)->get().
 *
 * Use this when you want to do a database-level operation, like read a list of all collections, or
 * drop a collection.
 *
 * It is guaranteed that the lock will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetDb {
    AutoGetDb(const AutoGetDb&) = delete;
    AutoGetDb& operator=(const AutoGetDb&) = delete;

public:
    /**
     * Acquires a lock on the specified database 'dbName' in the requested 'mode'.
     *
     * If the database belongs to a tenant, then acquires a tenant lock before the database lock.
     * For 'mode' MODE_IS or MODE_S acquires tenant lock in intent-shared (IS) mode, otherwise,
     * acquires a tenant lock in intent-exclusive (IX) mode.
     */
    AutoGetDb(OperationContext* opCtx,
              const DatabaseName& dbName,
              LockMode mode,
              Date_t deadline = Date_t::max());

    /**
     * Acquires a lock on the specified database 'dbName' in the requested 'mode'.
     *
     * If the database belongs to a tenant, then acquires a tenant lock before the database lock.
     * For 'mode' MODE_IS or MODE_S acquires tenant lock in intent-shared (IS) mode, otherwise,
     * acquires a tenant lock in intent-exclusive (IX) mode. A different, stronger tenant lock mode
     * to acquire can be specified with 'tenantLockMode' parameter. Passing boost::none for the
     * tenant lock mode does not skip the tenant lock, but indicates that the tenant lock in default
     * mode should be acquired.
     */
    AutoGetDb(OperationContext* opCtx,
              const DatabaseName& dbName,
              LockMode mode,
              boost::optional<LockMode> tenantLockMode,
              Date_t deadline = Date_t::max());

    AutoGetDb(OperationContext* opCtx,
              const DatabaseName& dbName,
              LockMode mode,
              boost::optional<LockMode> tenantLockMode,
              Date_t deadline,
              Lock::DBLockSkipOptions options);

    AutoGetDb(AutoGetDb&&) = default;

    static bool canSkipRSTLLock(const NamespaceStringOrUUID& nsOrUUID);
    static bool canSkipFlowControlTicket(const NamespaceStringOrUUID& nsOrUUID);

    static AutoGetDb createForAutoGetCollection(OperationContext* opCtx,
                                                const NamespaceStringOrUUID& nsOrUUID,
                                                LockMode modeColl,
                                                const auto_get_collection::Options& options);

    /**
     * Returns the database, or nullptr if it didn't exist.
     */
    Database* getDb() const {
        return _db;
    }

    /**
     * Returns the database, creating it if it does not exist.
     */
    Database* ensureDbExists(OperationContext* opCtx);

    /**
     * Returns the database reference, after attempting to refresh it if it was null. Does not
     * create the database, so after this call the referece might still be null.
     */
    Database* refreshDbReferenceIfNull(OperationContext* opCtx);

private:
    DatabaseName _dbName;

    // Special note! The primary DBLock must destruct last (be declared first) so that the global
    // and RSTL locks are not released until all the secondary DBLocks (without global and RSTL)
    // have destructed.
    Lock::DBLock _dbLock;

    Database* _db;

    // The secondary DBLocks will be acquired without the global or RSTL locks taken, re: the
    // skipGlobalAndRSTLLocks flag in the DBLock constructor.
    std::vector<Lock::DBLock> _secondaryDbLocks;
};

/**
 * Light wrapper around Lock::CollectionLock which allows acquiring the lock based on UUID rather
 * than namespace.
 *
 * The lock manager manages resources based on namespace and does not have a concept of UUIDs, so
 * there must be some additional concurrency checks around resolving the UUID to a namespace and
 * then subsequently acquiring the lock.
 */
class CollectionNamespaceOrUUIDLock {
public:
    CollectionNamespaceOrUUIDLock(OperationContext* opCtx,
                                  const NamespaceStringOrUUID& nsOrUUID,
                                  LockMode mode,
                                  Date_t deadline = Date_t::max());

    CollectionNamespaceOrUUIDLock(CollectionNamespaceOrUUIDLock&& other) = default;

    static Lock::CollectionLock resolveAndLockCollectionByNssOrUUID(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        LockMode mode,
        Date_t deadline = Date_t::max());

private:
    Lock::CollectionLock _lock;
};

/**
 * RAII-style class, which acquires global, database, and collection locks according to the chart
 * below.
 *
 * | modeColl | Global Lock Result | DB Lock Result | Collection Lock Result |
 * |----------+--------------------+----------------+------------------------|
 * | MODE_IX  | MODE_IX            | MODE_IX        | MODE_IX                |
 * | MODE_X   | MODE_IX            | MODE_IX        | MODE_X                 |
 * | MODE_IS  | MODE_IS            | MODE_IS        | MODE_IS                |
 * | MODE_S   | MODE_IS            | MODE_IS        | MODE_S                 |
 *
 * NOTE: Throws NamespaceNotFound if the collection UUID cannot be resolved to a name.
 *
 * Any acquired locks may be released when this object goes out of scope, therefore the database
 * and the collection references returned by this class should not be retained.
 */
class AutoGetCollection {
    AutoGetCollection(const AutoGetCollection&) = delete;
    AutoGetCollection& operator=(const AutoGetCollection&) = delete;

public:
    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      LockMode modeColl,
                      const auto_get_collection::Options& options = {});

    AutoGetCollection(AutoGetCollection&&) = default;
    AutoGetCollection() = default;
    explicit operator bool() const {
        return static_cast<bool>(_coll);
    }

    /**
     * AutoGetCollection can be used as a pointer with the -> operator.
     */
    const Collection* operator->() const {
        return _coll.get();
    }

    const CollectionPtr& operator*() const {
        return _coll;
    }

    /**
     * Returns the database, or nullptr if it didn't exist.
     */
    Database* getDb() const {
        return _autoDb.getDb();
    }

    /**
     * Returns the database, creating it if it does not exist.
     */
    Database* ensureDbExists(OperationContext* opCtx) {
        return _autoDb.ensureDbExists(opCtx);
    }

    /**
     * Returns nullptr if the view didn't exist.
     */
    const ViewDefinition* getView() const {
        return _view.get();
    }

    /**
     * Returns the resolved namespace of the collection or view.
     */
    const NamespaceString& getNss() const {
        return _resolvedNss;
    }

protected:
    friend class CollectionWriter;

    AutoGetCollection(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      LockMode modeColl,
                      const auto_get_collection::Options& options,
                      bool verifyWriteEligible);
    // Ordering matters, the _collLocks should destruct before the _autoGetDb releases the
    // rstl/global/database locks.
    AutoGetDb _autoDb;
    std::vector<CollectionNamespaceOrUUIDLock> _collLocks;

    CollectionPtr _coll;
    std::shared_ptr<const ViewDefinition> _view;

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string
    NamespaceString _resolvedNss;
};

class CollectionAcquisition;
class ScopedLocalCatalogWriteFence;

/**
 * RAII-style class to handle the lifetime of writable Collections.
 * It does not take any locks, concurrency needs to be handled separately using explicit locks or
 * AutoGetCollection. This class can serve as an adaptor to unify different methods of acquiring a
 * writable collection.
 *
 * It is safe to re-use an instance for multiple WriteUnitOfWorks. It is not safe to destroy it
 * before the active WriteUnitOfWork finishes.
 */
class CollectionWriter final {
public:
    // This constructor indicates to the shard role subsystem that the subsequent code enters into
    // local DDL land and that the content of the local collection should not be trusted until it
    // goes out of scope.
    //
    // On destruction, if `getWritableCollection` been called during the object lifetime, the
    // `acquisition` will be advanced to reflect the local catalog changes. It is important that
    // when this destructor is called, the WUOW under which the catalog changes have been performed
    // has already been commited or rollbacked. If it hasn't and the WUOW later rollbacks, the
    // acquisition is left in an invalid state and must not be used.
    //
    // Example usage pattern:
    // writeConflictRetry {
    //     auto coll = acquireCollection(...);
    //     CollectionWriter collectionWriter(opCtx, &coll);
    //     WriteUnitOfWork wuow();
    //     collectionWriter.getWritableCollection().xxxx();
    //     wouw.commit();
    // }
    //
    // Example usage pattern when the acquisition is held higher up by the caller:
    // auto coll = acquireCollection(...);
    // ...
    // writeConflictRetry {
    //     // It is important that ~CollectionWriter will be executed after the ~WriteUnitOfWork
    //     // commits or rollbacks.
    //     CollectionWriter collectionWriter(opCtx, &coll);
    //     WriteUnitOfWork wuow();
    //     collectionWriter.getWritableCollection().xxxx();
    //     wouw.commit();
    // }
    //
    // TODO (SERVER-73766): Only this constructor should remain in use
    CollectionWriter(OperationContext* opCtx, CollectionAcquisition* acquisition);

    // Gets the collection from the catalog for the provided uuid
    CollectionWriter(OperationContext* opCtx, const UUID& uuid);
    // Gets the collection from the catalog for the provided namespace string
    CollectionWriter(OperationContext* opCtx, const NamespaceString& nss);
    // Acts as an adaptor for AutoGetCollection
    CollectionWriter(OperationContext* opCtx, AutoGetCollection& autoCollection);
    // Acts as an adaptor for a writable Collection that has been retrieved elsewhere. This
    // 'CollectionWriter' will become 'unmanaged' where the Collection will not be updated on
    // commit/rollback.
    CollectionWriter(Collection* writableCollection);

    ~CollectionWriter();

    // Not allowed to copy or move.
    CollectionWriter(const CollectionWriter&) = delete;
    CollectionWriter(CollectionWriter&&) = delete;
    CollectionWriter& operator=(const CollectionWriter&) = delete;
    CollectionWriter& operator=(CollectionWriter&&) = delete;

    explicit operator bool() const {
        return static_cast<bool>(get());
    }

    const Collection* operator->() const {
        return get().get();
    }

    const Collection& operator*() const {
        return *get().get();
    }

    const CollectionPtr& get() const {
        return _storedCollection;
    }

    // Returns writable Collection, any previous Collection that has been returned may be
    // invalidated.
    Collection* getWritableCollection(OperationContext* opCtx);

private:
    // This group of values is only operated on for code paths that go through the
    // `CollectionAcquisition` constructor.
    CollectionAcquisition* _acquisition = nullptr;
    std::unique_ptr<ScopedLocalCatalogWriteFence> _fence;

    // Points to the current collection instance to use that is either the old read-only instance or
    // the one we're modyfing.
    CollectionPtr _storedCollection;
    Collection* _writableCollection = nullptr;

    // Indicates if this instance is managing Collection pointers through commit and rollback.
    bool _managed;

    struct SharedImpl;
    std::shared_ptr<SharedImpl> _sharedImpl;
};

/**
 * Writes to system.views need to use a stronger lock to prevent inconsistencies like view cycles.
 */
LockMode fixLockModeForSystemDotViewsChanges(const NamespaceString& nss, LockMode mode);

/**
 * RAII type to set and restore the timestamp read source on the recovery unit.
 *
 * Snapshot is abandoned in constructor and destructor, so it can only be used before
 * the recovery unit becomes active or when the existing snapshot is no longer needed.
 */
class ReadSourceScope {
public:
    ReadSourceScope(OperationContext* opCtx,
                    RecoveryUnit::ReadSource readSource,
                    boost::optional<Timestamp> provided = boost::none,
                    bool waitForOplog = false);
    ~ReadSourceScope();

private:
    OperationContext* _opCtx;
    RecoveryUnit::ReadSource _originalReadSource;
    Timestamp _originalReadTimestamp;
};

/**
 * RAII-style class to acquire the oplog with weaker consistency rules.
 *
 * IMPORTANT: this acquisition is optimized for fast-path access and is suitable for efficiently
 * reading from or writing to the oplog table. This acquisition can return a stale view of the
 * oplog metadata if interleaving with a DDL operation like an oplog resize. For consistent
 * lookups and support for yield and restore, use a conventional acquisition API like
 * mongo::acquireCollection.
 *
 * Only the global lock is acquired:
 * | OplogAccessMode | Global Lock |
 * +-----------------+-------------|
 * | kRead           | MODE_IS     |
 * | kWrite          | MODE_IX     |
 * | kLogOp          | -           |
 *
 * kLogOp is a special mode for replication operation logging and it behaves similar to kWrite. The
 * difference between kWrite and kLogOp is that kLogOp invariants that global IX lock is already
 * held. It is the caller's responsibility to ensure the global lock already held is still valid
 * within the lifetime of this object.
 *
 * The catalog resources are released when this object goes out of scope, therefore the oplog
 * collection reference returned by this class should not be retained.
 */
enum class OplogAccessMode { kRead, kWrite, kLogOp };

struct AutoGetOplogFastPathOptions {
    bool skipRSTLLock = false;
    boost::optional<rss::consensus::IntentRegistry::Intent> explicitIntent = boost::none;
};

class AutoGetOplogFastPath {
    AutoGetOplogFastPath(const AutoGetOplogFastPath&) = delete;
    AutoGetOplogFastPath& operator=(const AutoGetOplogFastPath&) = delete;

public:
    AutoGetOplogFastPath(
        OperationContext* opCtx,
        OplogAccessMode mode,
        Date_t deadline = Date_t::max(),
        const AutoGetOplogFastPathOptions& options = AutoGetOplogFastPathOptions());

    /**
     * Return a pointer to the per-service-context LocalOplogInfo.
     */
    LocalOplogInfo* getOplogInfo() const {
        return _oplogInfo;
    }

    /**
     * Returns a pointer to the oplog collection or nullptr if the oplog collection didn't exist.
     */
    const CollectionPtr& getCollection() const {
        return _oplog;
    }

private:
    boost::optional<Lock::GlobalLock> _globalLock;
    LocalOplogInfo* _oplogInfo;
    CollectionPtr _oplog;

    // Retain the CollectionCatalog snapshot since this fast-path acquisition skips acquiring the
    // oplog collection lock.
    std::shared_ptr<const CollectionCatalog> _stashedCatalog;
};

/**
 * A RAII-style class to acquire lock to a particular tenant's change collection.
 *
 * A change collection can be accessed in the following modes:
 *   kWriteInOplogContext - assumes that the tenant IX lock has been pre-acquired. The user can
 *                          perform reads and writes to the change collection.
 *   kWrite - behaves the same as 'AutoGetCollection::AutoGetCollection()' with lock mode MODE_IX.
 *   kUnreplicatedWrite - behaves the same as 'AutoGetCollection::AutoGetCollection()' with lock
 * mode MODE_IX and with explicit LocalWrite intent. kRead - behaves the same as
 * 'AutoGetCollection::AutoGetCollection()' with lock mode MODE_IS.
 */
class AutoGetChangeCollection {
public:
    enum class AccessMode { kWriteInOplogContext, kWrite, kUnreplicatedWrite, kRead };

    AutoGetChangeCollection(OperationContext* opCtx,
                            AccessMode mode,
                            const TenantId& tenantId,
                            Date_t deadline = Date_t::max());

    AutoGetChangeCollection(const AutoGetChangeCollection&) = delete;
    AutoGetChangeCollection& operator=(const AutoGetChangeCollection&) = delete;

    const Collection* operator->() const;
    const CollectionPtr& operator*() const;
    explicit operator bool() const;

private:
    // Used when the 'kWrite' or 'kRead' access mode is used.
    boost::optional<AutoGetCollection> _coll;
    // Used when the 'kWriteInOplogContext' access mode is used.
    CollectionPtr _changeCollection;
};

}  // namespace mongo
