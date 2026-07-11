// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

class CommandInvocation;
struct OpMsgRequest;

/**
 * MirrorMaestro coordinates commands received by a replica set member and potentially
 * sends a copy of the request to other members of replica set.
 *
 * All public functions are thread-safe.
 */
class [[MONGO_MOD_PUBLIC]] MirrorMaestro {
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

    static constexpr auto kServerStatusSectionName = "mirroredReads"sv;
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
void recomputeCachedHostsForTargetedMirroring_forTest(ServiceContext* serviceContext);

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
