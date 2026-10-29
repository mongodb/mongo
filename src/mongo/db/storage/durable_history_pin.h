// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_OPEN]] DurableHistoryPin {
public:
    virtual ~DurableHistoryPin() {}

    virtual std::string getName() = 0;

    virtual boost::optional<Timestamp> calculatePin(OperationContext* opCtx) = 0;
};

/**
 * Services that want to preserve storage engine history across restarts or replication rollback
 * should create a class that implements `DurableHistoryPin` and register an instance of that class
 * at startup prior to the first call of `reconcilePins`.
 */
class [[MONGO_MOD_PUBLIC]] DurableHistoryRegistry {
public:
    static DurableHistoryRegistry* get(ServiceContext* service);
    static DurableHistoryRegistry* get(ServiceContext& service);
    static DurableHistoryRegistry* get(OperationContext* ctx);

    static void set(ServiceContext* service, std::unique_ptr<DurableHistoryRegistry> registry);

    void registerPin(std::unique_ptr<DurableHistoryPin> pin);

    /**
     * Iterates through each registered pin and takes one of two actions:
     *
     * 1) If the pin returns an engaged boost::optional<Timestamp>, forward that pinned timestamp to
     *    the storage engine using the pins name.
     * 2) If the pin returns boost::none, unpin any resources held by the storage engine on behalf
     *    of the pins name.
     *
     * If a requested pin fails, a log will be issued, but the process will otherwise continue.
     */
    void reconcilePins(OperationContext* opCtx);

    /**
     * Removes all registered pins.
     */
    void clearPins(OperationContext* opCtx);

private:
    std::vector<std::unique_ptr<DurableHistoryPin>> _pins;
};

}  // namespace mongo
