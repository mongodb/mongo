/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
