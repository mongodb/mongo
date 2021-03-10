/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

/**
 * Every OperationContext is expected to have a unique OperationId within the domain of its
 * ServiceContext. Generally speaking, OperationId is used for forming maps of OperationContexts and
 * directing metaoperations like killop.
 */
using OperationId = uint32_t;

/**
 * This class issues guaranteed unique OperationIds for a given instance of this class.
 */
class UniqueOperationIdRegistry : public std::enable_shared_from_this<UniqueOperationIdRegistry> {
public:
    /**
     * This class represents a slot issued by a UniqueOperationIdRegistry.
     * It functions as an RAII wrapper for a unique OperationId.
     */
    class OperationIdSlot {
    public:
        explicit OperationIdSlot(OperationId id) : _id(id), _registry() {}

        OperationIdSlot(OperationId id, std::weak_ptr<UniqueOperationIdRegistry> registry)
            : _id(id), _registry(std::move(registry)) {}

        OperationIdSlot(OperationIdSlot&& other) = default;

        OperationIdSlot& operator=(OperationIdSlot&& other) noexcept {
            if (&other == this) {
                return *this;
            }
            _releaseSlot();
            _id = std::exchange(other._id, {});
            _registry = std::exchange(other._registry, {});
            return *this;
        }

        // Disable copies.
        OperationIdSlot(const OperationIdSlot&) = delete;
        OperationIdSlot& operator=(const OperationIdSlot&) = delete;

        ~OperationIdSlot() {
            _releaseSlot();
        }

        /**
         * Get the contained ID.
         */
        OperationId getId() const {
            return _id;
        }

    private:
        void _releaseSlot() {
            if (auto registry = _registry.lock()) {
                registry->_releaseSlot(_id);
            }
        }

        OperationId _id;
        std::weak_ptr<UniqueOperationIdRegistry> _registry;
    };

    /**
     * Public factory function.
     */
    static std::shared_ptr<UniqueOperationIdRegistry> create() {
        return std::shared_ptr<UniqueOperationIdRegistry>(new UniqueOperationIdRegistry());
    }

    /**
     * Gets a unique OperationIdSlot which will clear itself from the map when destroyed.
     */
    OperationIdSlot acquireSlot();

    /**
     * A helper class for exposing test functions.
     */
    class UniqueOperationIdRegistryTestHarness {
    public:
        /**
         * Returns true if the given operation ID exists.
         */
        static bool isActive(UniqueOperationIdRegistry& registry, OperationId id) {
            stdx::lock_guard lk(registry._mutex);
            return registry._activeIds.find(id) != registry._activeIds.end();
        }

        static void setNextOpId(UniqueOperationIdRegistry& registry, OperationId id) {
            stdx::lock_guard lk(registry._mutex);
            registry._nextOpId = id;
        }
    };

private:
    UniqueOperationIdRegistry() = default;

    /**
     * Clears a unique ID from the set.
     */
    void _releaseSlot(OperationId id);

    Mutex _mutex = MONGO_MAKE_LATCH("UniqueOperationIdRegistry::_mutex");
    stdx::unordered_set<OperationId> _activeIds;

    OperationId _nextOpId = 1U;
};

using OperationIdSlot = UniqueOperationIdRegistry::OperationIdSlot;

}  // namespace mongo
