// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/change_stream_options_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo {

/**
 * Manages fetching and storing of the change streams options. The class manages the complete read
 * and write path for the change-streams options.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChangeStreamOptionsManager {
public:
    explicit ChangeStreamOptionsManager(ServiceContext* service) {}

    ~ChangeStreamOptionsManager() = default;

    ChangeStreamOptionsManager(const ChangeStreamOptionsManager&) = delete;
    ChangeStreamOptionsManager& operator=(const ChangeStreamOptionsManager&) = delete;

    /**
     * Creates an instance of the class using the service-context.
     */
    static void create(ServiceContext* service);

    /**
     * Gets the instance of the class using the service context.
     */
    static ChangeStreamOptionsManager& get(ServiceContext* service);

    /**
     * Gets the instance of the class using the operation context.
     */
    static ChangeStreamOptionsManager& get(OperationContext* opCtx);

    /**
     * Returns the change-streams options.
     */
    ChangeStreamOptions getOptions(OperationContext* opCtx) const;

    /**
     * Sets the provided change-streams options. Returns OK on success, otherwise appropriate error
     * status is returned.
     */
    StatusWith<ChangeStreamOptions> setOptions(OperationContext* opCtx,
                                               ChangeStreamOptions optionsToSet);

    /**
     * Returns the clusterParameterTime of the current change stream options.
     */
    const LogicalTime& getClusterParameterTime() const;

private:
    ChangeStreamOptions _changeStreamOptions;

    mutable std::mutex _mutex;
};

}  // namespace mongo
