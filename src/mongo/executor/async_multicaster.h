// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace mongo {

class DefaultRetryStrategy;
class RetryStrategy;

namespace executor {

/**
 * An async harness for scatter/gathering a command across an arbitrary number of specific hosts
 */
class [[MONGO_MOD_PUBLIC]] AsyncMulticaster {
public:
    using Reply = std::tuple<HostAndPort, executor::RemoteCommandResponse>;
    static constexpr size_t kMaxConcurrency = 100;

    using StrategyFactory = std::function<std::unique_ptr<RetryStrategy>()>;

    struct Options {
        // maxConcurrency controls the maximum number of inflight operations.  I.e. limiting it
        // prevents the fan out from overwhelming the host, if the number of servers to multicast
        // to is very high.
        size_t maxConcurrency = kMaxConcurrency;

        // Factory function for creating RetryStrategy instances. By default, AsyncMulticaster uses
        // DefaultRetryStrategy, but this factory allows injecting custom retry strategy
        // implementations. The factory is called once per target host to ensure each host gets its
        // own strategy instance, enabling per-host retry state tracking (see the 'strategies' map
        // in State struct).
        StrategyFactory strategyFactory;
    };

    AsyncMulticaster(std::shared_ptr<executor::TaskExecutor> executor, Options options);

    /**
     * Sends the cmd out to all passed servers (via the executor), observing the multicaster's
     * maxConcurrency.
     *
     * The timeout value on multicast is per operation.  The overall timeout will be:
     *   timeoutMillis - if max concurrency is greater than servers.size()
     * or
     *   timeoutMillis * (servers.size() / maxConcurrency) - if not
     */
    std::vector<Reply> multicast(std::vector<HostAndPort> servers,
                                 const DatabaseName& theDbName,
                                 const BSONObj& theCmdObj,
                                 OperationContext* opCtx,
                                 Milliseconds timeoutMillis);

private:
    // Everything goes into a state struct because we can get cancelled, and then our callback would
    // be invoked later.
    struct State {
        State(size_t leftToDo) : leftToDo(leftToDo) {}

        std::mutex mutex;
        stdx::condition_variable cv;
        size_t leftToDo;
        size_t running = 0;

        // To indicate which hosts fail.
        std::vector<Reply> out;

        // Per-host retry strategies. Each target host maintains its own RetryStrategy instance
        // to track independent retry state (attempt counts, backoff timers, etc.) across
        // concurrent operations.
        absl::flat_hash_map<HostAndPort, std::unique_ptr<RetryStrategy>> strategies;
    };

    Options _options;
    std::shared_ptr<executor::TaskExecutor> _executor;

    void _scheduleAttempt(std::shared_ptr<State> state,
                          const RemoteCommandRequest& request,
                          const CancellationToken& cancellationToken,
                          const HostAndPort& server);

    void _onComplete(WithLock,
                     std::shared_ptr<State> state,
                     const HostAndPort& server,
                     const RemoteCommandResponse& response);
};

}  // namespace executor
}  // namespace mongo
