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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <map>
#include <string>

MONGO_MOD_PUBLIC;

namespace mongo {

/**
 * Tracks per-action duration metrics.
 *
 * This is a ServiceContext decoration.
 */
class ActionDurationMetrics {
public:
    struct Entry {
        Milliseconds last{0};
        Milliseconds total{0};
        int64_t count{0};
    };

    static const ActionDurationMetrics& getDecoration(ServiceContext* serviceContext);

    void record(const std::string& action, Milliseconds millis);

    void report(BSONObjBuilder* builder) const;

private:
    mutable stdx::mutex _mutex;

    // Use an ordered map to prevent schema changes in FTDC.
    std::map<std::string, Entry> _entries;
};

/**
 * RAII helper that measures the duration of a named action and records it into the
 * ActionDurationMetrics decoration.
 */
class ActionDurationTimer {
public:
    ActionDurationTimer(const ActionDurationTimer&) = delete;
    ActionDurationTimer& operator=(const ActionDurationTimer&) = delete;
    ActionDurationTimer(ActionDurationTimer&&) = delete;
    ActionDurationTimer& operator=(ActionDurationTimer&&) = delete;

    // Caller is responsible for using the RAII within the OperationContext/ServiceContext lifetime.
    ActionDurationTimer(OperationContext* opCtx, std::string action);
    ActionDurationTimer(ServiceContext* serviceContext, std::string action);

    ~ActionDurationTimer();

private:
    ServiceContext* _serviceContext;
    std::string _action;
    Date_t _start;
};

}  // namespace mongo
