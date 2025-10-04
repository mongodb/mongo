/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/health_log_gen.h"
#include "mongo/db/service_context.h"

#include <cstdint>
#include <memory>

namespace mongo {

/**
 * The interface to the local healthlog.
 *
 * This class contains facilities for creating and asynchronously writing to the local healthlog
 * collection. There should only be one instance of this class, initialized on startup and cleaned
 * up on shutdown.
 */
class HealthLogInterface {
    HealthLogInterface(const HealthLogInterface&) = delete;
    HealthLogInterface& operator=(const HealthLogInterface&) = delete;

public:
    /**
     * The maximum size of the in-memory buffer of health-log entries, in bytes.
     */
    static const int64_t kMaxBufferSize = 25'000'000;

    /**
     * Stores a health log on the specified service context. May only be called once for the
     * lifetime of the service context.
     */
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<HealthLogInterface> newHealthLog);

    /**
     * Get the current context's HealthLog. set() above must be called before any get() calls.
     */
    static HealthLogInterface* get(ServiceContext* ctx);
    static HealthLogInterface* get(OperationContext* ctx);

    /**
     * Required to use HealthLogInterface as a ServiceContext decorator.
     *
     * Should not be used anywhere else.
     */
    HealthLogInterface() = default;
    virtual ~HealthLogInterface() = default;

    /**
     * Start the worker thread writing the buffer to the collection.
     */
    virtual void startup() = 0;

    /**
     * Stop the worker thread.
     */
    virtual void shutdown() = 0;

    /**
     * Asynchronously insert the given entry.
     *
     * Return `false` iff there is no more space in the buffer.
     */
    virtual bool log(const HealthLogEntry& entry) = 0;
};
}  // namespace mongo
