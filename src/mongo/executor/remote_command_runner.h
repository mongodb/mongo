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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include <memory>

namespace mongo {
namespace executor {
namespace remote_command_runner {


namespace detail {
/**
 * Executes the BSON command asynchronously on the given target.
 *
 * Do not call directly - this is not part of the public API.
 */
ExecutorFuture<BSONObj> _doRequest(StringData dbName,
                                   BSONObj cmdBSON,
                                   HostAndPort target,
                                   OperationContext* opCtx,
                                   std::shared_ptr<executor::TaskExecutor> exec,
                                   CancellationToken token);
}  // namespace detail

/**
 * Execute the command asynchronously on the given target with the provided executor.
 * Returns a SemiFuture with the reply from the IDL command, or throws an error.
 */
template <typename CommandType>
SemiFuture<typename CommandType::Reply> doRequest(CommandType cmd,
                                                  OperationContext* opCtx,
                                                  std::shared_ptr<executor::TaskExecutor> exec,
                                                  CancellationToken token) {
    const HostAndPort target = HostAndPort("FakeShard1Host", 12345);

    /* Execute the command after extracting the db name and bson from the CommandType. Wrapping this
     * function allows us to seperate the CommandType parsing logic from the implementation details
     * of executing the remote command asynchronously.
     */
    auto resFuture =
        detail::_doRequest(cmd.getDbName(), cmd.toBSON({}), target, opCtx, exec, token);

    return std::move(resFuture)
        .then([&, exec = std::move(exec)](BSONObj r) {
            // TODO SERVER-67661: Make IDL reply types have string representation for logging
            auto res = CommandType::Reply::parse(IDLParserContext("RemoteCommandRunner"), r);
            return res;
        })
        .semi();
}

}  // namespace remote_command_runner
}  // namespace executor
}  // namespace mongo
