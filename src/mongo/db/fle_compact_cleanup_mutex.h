// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <utility>

namespace mongo {

class FLECompactCleanupMutex;
class OperationContext;
class ServiceContext;

/**
 * Serializes FLE compact and cleanup operations for the same EDC using an in-memory mutex keyed by
 * the derived 'ecoc.lock' namespace. This mutex intentionally lives outside the locker/transaction
 * resource machinery so it remains effective across the internal transaction stash/restore flow.
 */
class [[MONGO_MOD_PUBLIC]] ScopedFLECompactCleanupMutex {
public:
    ScopedFLECompactCleanupMutex(OperationContext* opCtx, const NamespaceString& ecocLockNss);
    ~ScopedFLECompactCleanupMutex();

    ScopedFLECompactCleanupMutex(const ScopedFLECompactCleanupMutex&) = delete;
    ScopedFLECompactCleanupMutex& operator=(const ScopedFLECompactCleanupMutex&) = delete;

private:
    OperationContext* _opCtx;
    NamespaceString _ecocLockNss;
    std::shared_ptr<FLECompactCleanupMutex> _mutex;
};

[[MONGO_MOD_PUBLIC]] size_t getFLECompactCleanupMutexRegistrySizeForTest(
    ServiceContext* serviceContext);

template <typename GetNamespaces>
[[MONGO_MOD_PUBLIC]] auto acquireFLECompactCleanupMutexWithStableNamespaces(
    OperationContext* opCtx, GetNamespaces getNamespaces) {
    auto namespaces = getNamespaces();
    while (true) {
        auto scopedMutex =
            std::make_unique<ScopedFLECompactCleanupMutex>(opCtx, namespaces.ecocLockNss);

        auto latestNamespaces = getNamespaces();
        if (latestNamespaces.ecocLockNss == namespaces.ecocLockNss) {
            return std::pair{std::move(latestNamespaces), std::move(scopedMutex)};
        }

        namespaces = std::move(latestNamespaces);
    }
}

}  // namespace mongo
