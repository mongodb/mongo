/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

#include <cstddef>

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
class OperationKeyManager {
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
    mutable stdx::mutex _mutex;

    stdx::unordered_map<OperationKey, OperationId, OperationKey::Hash> _idByOperationKey;
};

}  // namespace mongo
