/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/db/change_stream_options_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

/**
 * Manages fetching and storing of the change streams options. The class manages the complete read
 * and write path for the change-streams options.
 */
class ChangeStreamOptionsManager {
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

    mutable stdx::mutex _mutex;
};

}  // namespace mongo
