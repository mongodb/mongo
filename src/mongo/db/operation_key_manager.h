// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <mutex>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * OperationKeyManager maps OperationKeys to OperationIds
 *
 * To make this simpler to reason about, this class is a decoration on a ServiceContext with its own
 * synchronization. This class is currently expected to be used at 3 points:
 * - Before Command execution, the OperationKey from the client application is added.
 * - During a killop-like operation, it is used to find the OperationContext to kill.
 * - During OperationContext destruction, the OperationKey from the client application is removed.
 */
class [[MONGO_MOD_PUBLIC]] OperationKeyManager {
public:
    static OperationKeyManager& get(ServiceContext* serviceContext = getCurrentServiceContext());
    static OperationKeyManager& get(Client* client) {
        return get(client->getServiceContext());
    }
    static OperationKeyManager& get(OperationContext* opCtx) {
        return get(opCtx->getServiceContext());
    }

    ~OperationKeyManager();

    /**
     * Add a mapping from OperationKey to OperationId
     */
    void add(const OperationKey& key, OperationId id);

    /**
     * Remove any mapping from OperationKey
     */
    bool remove(const OperationKey& key);

    /**
     * Get the OperationId for OperationKey, or boost::none
     */
    boost::optional<OperationId> at(const OperationKey& key) const;

    /**
     * Get the total count of OperationKeys currently being tracked
     */
    size_t size() const;

private:
    mutable std::mutex _mutex;

    stdx::unordered_map<OperationKey, OperationId, OperationKey::Hash> _idByOperationKey;
};

}  // namespace mongo
