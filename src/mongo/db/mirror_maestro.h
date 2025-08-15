/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"

#include <memory>
#include <vector>

namespace mongo {

class CommandInvocation;
struct OpMsgRequest;

/**
 * MirrorMaestro coordinates commands received by a replica set member and potentially
 * sends a copy of the request to other members of replica set.
 *
 * All public functions are thread-safe.
 */
class MirrorMaestro {
public:
    /**
     * Initialize the MirrorMaestro for serviceContext
     *
     * This function blocks until the MirrorMaestro is available.
     */
    static void init(ServiceContext* serviceContext);

    /**
     * Shutdown the MirrorMaestro for serviceContext
     *
     * This function blocks until the MirrorMaestro is no longer available.
     */
    static void shutdown(ServiceContext* serviceContext);

    /**
     * Check if the request associated with opCtx should be mirrored to secondaries, and schedule
     * that work if so.
     *
     * This function will noop if the MirrorMaestro is currently being initialized or shutdown.
     */
    static void tryMirrorRequest(OperationContext* opCtx);

    /**
     * Runs custom logic as part of receiving a mirrored operation.
     */
    static void onReceiveMirroredRead(OperationContext* opCtx);

    static constexpr auto kServerStatusSectionName = "mirroredReads"_sd;
};

/**
 * Utility functions used for testing that expose internal functionality.
 */

/**
 * Returns the cached HelloResponse that will be used for general mirrored reads.
 */
StatusWith<std::shared_ptr<const repl::HelloResponse>> getCachedHelloResponse_forTest(
    ServiceContext* serviceContext);

/**
 * Returns the list of hosts that will be used for targeted mirrored reads. Returns a
 * NotYetInitialized status if the MirrorMaestro isn't initialized.
 */
StatusWith<std::vector<HostAndPort>> getCachedHostsForTargetedMirroring_forTest(
    ServiceContext* serviceContext);

/**
 * Updates the list of hosts that will be used by targeted mirrored reads.
 *
 * The function will update the list of hosts to target on config version changes, or if the repl
 * set tag used to target hosts is updated (tagChanged).
 */
void updateCachedHostsForTargetedMirroring_forTest(ServiceContext* serviceContext,
                                                   const repl::ReplSetConfig& replSetConfig,
                                                   bool tagChanged);

/**
 * Gets the executor associated with the MirrorMaestro. Returns a NotYetInitialized status if the
 * MirrorMaestro isn't initialized.
 */
StatusWith<std::shared_ptr<executor::TaskExecutor>> getMirroringTaskExecutor_forTest(
    ServiceContext* serviceContext);

/**
 * Sets the executor associated with the MirrorMaestro.
 *
 * Note: This may only be called after initializing the MirrorMaestro.
 */
void setMirroringTaskExecutor_forTest(ServiceContext* serviceContext,
                                      std::shared_ptr<executor::TaskExecutor> executor);

}  // namespace mongo
