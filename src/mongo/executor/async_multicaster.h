/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace mongo {
namespace executor {

/**
 * An async harness for scatter/gathering a command across an arbitrary number of specific hosts
 */
class AsyncMulticaster {
public:
    using Reply = std::tuple<HostAndPort, executor::RemoteCommandResponse>;
    static constexpr size_t kMaxConcurrency = 100;

    struct Options {
        // maxConcurrency controls the maximum number of inflight operations.  I.e. limiting it
        // prevents the fan out from overwhelming the host, if the number of servers to multicast
        // to is very high.
        size_t maxConcurrency = kMaxConcurrency;
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
    Options _options;
    std::shared_ptr<executor::TaskExecutor> _executor;
};

}  // namespace executor
}  // namespace mongo
