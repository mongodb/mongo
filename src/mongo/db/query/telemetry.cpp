/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/telemetry.h"

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/memory_util.h"
#include "mongo/util/processinfo.h"

namespace mongo {

namespace {

/**
 * A manager for the telemetry store allows a "pointer swap" on the telemetry store itself. The
 * usage patterns are as follows:
 *
 * - Updating the telemetry store uses the `getTelemetryStore()` method. The telemetry store
 *   instance is obtained, entries are looked up and mutated, or created anew.
 * - The telemetry store is "reset". This involves atomically allocating a new instance, once there
 *   are no more updaters (readers of the store "pointer"), and returning the existing instance.
 */
class TelemetryStoreManager {
public:
    template <typename... TelemetryStoreArgs>
    TelemetryStoreManager(ServiceContext* serviceContext, TelemetryStoreArgs... args)
        : _telemetryStore(
              std::make_unique<TelemetryStore>(std::forward<TelemetryStoreArgs>(args)...)),
          _instanceLock(LockerImpl{serviceContext}),
          _instanceMutex("TelemetryStoreManager") {}

    /**
     * Acquire the instance of the telemetry store. The telemetry store is mutable and a shared
     * "read lock" is obtained on the instance. That is, the telemetry store instance will not be
     * replaced.
     */
    std::pair<TelemetryStore*, Lock::ResourceLock> getTelemetryStore() {
        return std::make_pair(&*_telemetryStore, Lock::SharedLock{&_instanceLock, _instanceMutex});
    }

    /**
     * Acquire the instance of the telemetry store at the same time atomically replacing the
     * internal instance with a new instance. This operation acquires an exclusive "write lock"
     * which waits for all read locks to be released before replacing the instance.
     */
    std::unique_ptr<TelemetryStore> resetTelemetryStore() {
        Lock::ExclusiveLock writeLock{&_instanceLock, _instanceMutex};
        auto oldStore = std::move(_telemetryStore);
        _telemetryStore = std::make_unique<TelemetryStore>(_telemetryStore->size(),
                                                           _telemetryStore->numPartitions());
        return oldStore;
    }

private:
    std::unique_ptr<TelemetryStore> _telemetryStore;

    /**
     * Lock over the telemetry store.
     */
    LockerImpl _instanceLock;

    Lock::ResourceMutex _instanceMutex;
};

const auto telemetryStoreDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<TelemetryStoreManager>>();

ServiceContext::ConstructorActionRegisterer telemetryStoreManagerRegisterer{
    "TelemetryStoreManagerRegisterer", [](ServiceContext* serviceCtx) {
        // TODO SERVER-71065 update telemetry cache size
        (void)internalQueryDisableExclusionProjectionFastPath;
        (void)queryTelemetrySamplingRate;
        (void)queryTelemetryCacheSize;

        auto size = 100000;  // getPlanCacheSizeInBytes(queryTelemetryCacheSize.get());
        auto&& globalTelemetryStoreManager = telemetryStoreDecoration(serviceCtx);
        globalTelemetryStoreManager =
            std::make_unique<TelemetryStoreManager>(serviceCtx, size, ProcessInfo::getNumCores());
    }};

}  // namespace

std::pair<TelemetryStore*, Lock::ResourceLock> getTelemetryStoreForRead(
    ServiceContext* serviceCtx) {
    return telemetryStoreDecoration(serviceCtx)->getTelemetryStore();
}

std::unique_ptr<TelemetryStore> resetTelemetryStore(ServiceContext* serviceCtx) {
    return telemetryStoreDecoration(serviceCtx)->resetTelemetryStore();
}

namespace telemetry_util {

Status onTelemetryCacheSizeUpdate(const std::string& str) {
    auto newSize = memory_util::MemorySize::parse(str);
    if (!newSize.isOK()) {
        return newSize.getStatus();
    }

    // TODO SERVER-71065 update telemetry cache size

    return Status::OK();
}

Status validateTelemetryCacheSize(const std::string& str, const boost::optional<TenantId>&) {
    return memory_util::MemorySize::parse(str).getStatus();
}

}  // namespace telemetry_util
}  // namespace mongo
