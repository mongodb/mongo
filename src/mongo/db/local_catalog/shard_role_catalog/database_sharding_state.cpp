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

#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"

#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

#include <memory>
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
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        auto it = _databases.find(dbName);
        if (it == _databases.end()) {
            auto inserted = _databases.try_emplace(
                dbName, std::make_unique<DSSAndLock>(_factory->make(dbName)));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return it->second.get();
    }

    std::vector<DatabaseName> getDatabaseNames() {
        stdx::lock_guard lg(_mutex);
        std::vector<DatabaseName> result;
        result.reserve(_databases.size());
        for (const auto& [dbName, _] : _databases) {
            result.emplace_back(dbName);
        }
        return result;
    }

private:
    std::unique_ptr<DatabaseShardingStateFactory> _factory;

    stdx::mutex _mutex;

    // Entries of the _databases map must never be deleted or replaced. This is to guarantee that a
    // 'dbName' is always associated to the same 'ResourceMutex'.
    using DatabasesMap = stdx::unordered_map<DatabaseName, std::unique_ptr<DSSAndLock>>;
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
