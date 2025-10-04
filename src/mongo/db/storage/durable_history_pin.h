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

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class DurableHistoryPin {
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
class DurableHistoryRegistry {
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
