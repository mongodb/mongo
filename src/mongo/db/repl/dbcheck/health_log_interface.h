// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/dbcheck/health_log_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

[[MONGO_MOD_PUBLIC]];

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
