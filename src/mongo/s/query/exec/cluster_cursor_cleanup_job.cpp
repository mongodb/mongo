// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/cluster_cursor_cleanup_job.h"

#include "mongo/db/client.h"
#include "mongo/db/query/client_cursor/cursor_server_params.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit.h"
#include "mongo/util/time_support.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

ClusterCursorCleanupJob clusterCursorCleanupJob;

std::string ClusterCursorCleanupJob::name() const {
    return "ClusterCursorCleanupJob";
}

void ClusterCursorCleanupJob::run() {
    ThreadClient tc(name(), getGlobalServiceContext()->getService());
    auto* const client = Client::getCurrent();
    auto* const manager = Grid::get(client->getServiceContext())->getCursorManager();
    invariant(manager);

    while (!globalInShutdownDeprecated()) {
        // Mirroring the behavior in CursorManager::timeoutCursors(), a negative value for
        // cursorTimeoutMillis has the same effect as a 0 value: cursors are cleaned immediately.
        auto cursorTimeoutValue = getCursorTimeoutMillis();
        const auto opCtx = client->makeOperationContext();
        Date_t cutoff = (cursorTimeoutValue > 0)
            ? (Date_t::now() - Milliseconds(cursorTimeoutValue))
            : Date_t::now();
        try {
            manager->killMortalCursorsInactiveSince(opCtx.get(), cutoff);
        } catch (const DBException& e) {
            LOGV2_WARNING(7466200,
                          "Cursor clean up encountered unexpected error, will retry after cursor "
                          "monitor interval",
                          "error"_attr = e.toString());
        }

        MONGO_IDLE_THREAD_BLOCK;
        sleepsecs(getClientCursorMonitorFrequencySecs());
    }
}

}  // namespace mongo
