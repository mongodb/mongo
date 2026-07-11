// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"

#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <mutex>

namespace mongo {
namespace {

class DatabaseShardingStateMap {
    DatabaseShardingStateMap& operator=(const DatabaseShardingStateMap&) = delete;
    DatabaseShardingStateMap(const DatabaseShardingStateMap&) = delete;

public:
    static const ServiceContext::Decoration<boost::optional<DatabaseShardingStateMap>> get;


    DatabaseShardingStateMap(std::unique_ptr<DatabaseShardingStateFactory> factory)
        : _factory(std::move(factory)) {}

    struct DSSAndLock {
        DSSAndLock(std::unique_ptr<DatabaseShardingState> dss) : dss(std::move(dss)) {}

        std::shared_mutex dssMutex;  // NOLINT
        std::unique_ptr<DatabaseShardingState> dss;
    };

    DSSAndLock* getOrCreate(const DatabaseName& dbName) {
        std::shared_lock readLock(_mutex);  // NOLINT
        if (auto it = _databases.find(dbName); MONGO_likely(it != _databases.end())) {
            return &it->second;
        }
        readLock.unlock();
        std::lock_guard writeLock(_mutex);
        auto [it, _] = _databases.emplace(dbName, _factory->make(dbName));
        return &it->second;
    }

    std::vector<DatabaseName> getDatabaseNames() {
        std::shared_lock lk(_mutex);  // NOLINT
        std::vector<DatabaseName> result;
        result.reserve(_databases.size());
        for (const auto& [dbName, _] : _databases) {
            result.emplace_back(dbName);
        }
        return result;
    }

    const StaleShardDatabaseMetadataHandler& getStaleShardExceptionHandler() {
        return _factory->getStaleShardExceptionHandler();
    }

private:
    std::unique_ptr<DatabaseShardingStateFactory> _factory;

    // Adding entries to `_databases` is expected to be very infrequent and far apart (first
    // database access), so the majority of accesses to this map are read-only and benefit from
    // using a shared mutex type for synchronization.
    mutable RWMutex _mutex;

    // Entries of the _databases map must never be deleted or replaced. This is to guarantee that a
    // 'dbName' is always associated to the same 'dssMutex'.
    using DatabasesMap = stdx::unordered_map<DatabaseName, DSSAndLock>;
    DatabasesMap _databases;
};

const ServiceContext::Decoration<boost::optional<DatabaseShardingStateMap>>
    DatabaseShardingStateMap::get =
        ServiceContext::declareDecoration<boost::optional<DatabaseShardingStateMap>>();

}  // namespace

DatabaseShardingState::ScopedDatabaseShardingState::ScopedDatabaseShardingState(
    LockType lock, DatabaseShardingState* dss)
    : _lock(std::move(lock)), _dss(dss) {}

DatabaseShardingState::ScopedDatabaseShardingState::ScopedDatabaseShardingState(
    ScopedDatabaseShardingState&& other)
    : _lock(std::move(other._lock)), _dss(other._dss) {
    other._dss = nullptr;
}

DatabaseShardingState::ScopedDatabaseShardingState::~ScopedDatabaseShardingState() = default;

DatabaseShardingState::ScopedDatabaseShardingState
DatabaseShardingState::ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(
    OperationContext* opCtx, const DatabaseName& dbName, LockMode mode) {
    // Only IS and X modes are supported.
    invariant(mode == MODE_IS || mode == MODE_X);

    DatabaseShardingStateMap::DSSAndLock* dssAndLock =
        DatabaseShardingStateMap::get(opCtx->getServiceContext())->getOrCreate(dbName);

    // First lock the shared_mutex associated to this nss to guarantee stability of the
    // DatabaseShadingState* . After that, it is safe to get and store the
    // DatabaseShadingState*, as long as the mutex is kept locked.
    if (mode == MODE_IS) {
        return ScopedDatabaseShardingState{std::shared_lock(dssAndLock->dssMutex),  // NOLINT
                                           dssAndLock->dss.get()};
    }

    return ScopedDatabaseShardingState{std::unique_lock(dssAndLock->dssMutex),  // NOLINT
                                       dssAndLock->dss.get()};
}

DatabaseShardingState::ScopedDatabaseShardingState DatabaseShardingState::acquire(
    OperationContext* opCtx, const DatabaseName& dbName) {
    return ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(opCtx, dbName, MODE_IS);
}

DatabaseShardingState::ScopedDatabaseShardingState DatabaseShardingState::assertDbLockedAndAcquire(
    OperationContext* opCtx, const DatabaseName& dbName) {
    dassert(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IS));
    return ScopedDatabaseShardingState::acquireScopedDatabaseShardingState(opCtx, dbName, MODE_IS);
}

std::vector<DatabaseName> DatabaseShardingState::getDatabaseNames(OperationContext* opCtx) {
    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return databasesMap->getDatabaseNames();
}

const StaleShardDatabaseMetadataHandler& DatabaseShardingState::getStaleShardExceptionHandler(
    OperationContext* opCtx) {
    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return databasesMap->getStaleShardExceptionHandler();
}

void DatabaseShardingStateFactory::set(ServiceContext* service,
                                       std::unique_ptr<DatabaseShardingStateFactory> factory) {
    auto& databasesMap = DatabaseShardingStateMap::get(service);
    invariant(!databasesMap);
    invariant(factory);
    databasesMap.emplace(std::move(factory));
}

void DatabaseShardingStateFactory::clear(ServiceContext* service) {
    if (auto& databasesMap = DatabaseShardingStateMap::get(service))
        databasesMap.reset();
}

}  // namespace mongo
