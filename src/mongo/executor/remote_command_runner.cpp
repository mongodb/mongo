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

#include "mongo/executor/remote_command_runner.h"

namespace mongo {
namespace executor {
namespace remote_command_runner {
namespace detail {
ExecutorFuture<BSONObj> _doRequest(StringData dbName,
                                   BSONObj cmdBSON,
                                   HostAndPort target,
                                   OperationContext* opCtx,
                                   std::shared_ptr<executor::TaskExecutor> exec,
                                   CancellationToken token) {
    executor::RemoteCommandRequest executorRequest(
        target, dbName.toString(), cmdBSON, rpc::makeEmptyMetadata(), opCtx);
    ExecutorFuture<executor::RemoteCommandResponse> f =
        exec->scheduleRemoteCommand(executorRequest, token);
    return std::move(f).then([&, exec = std::move(exec)](executor::RemoteCommandResponse r) {
        uassertStatusOK(r.status);                            // check local error
        uassertStatusOK(getStatusFromCommandResult(r.data));  // check remote error
        uassertStatusOK(
            getWriteConcernStatusFromCommandResult(r.data));  // check remote write concern error
        uassertStatusOK(
            getFirstWriteErrorStatusFromCommandResult(r.data));  // check remote write error

        // TODO SERVER-67649: Teach IDL to accept generic reply fields when parsing a reply.
        BSONObj newR = r.data.removeField("ok");

        return newR;
    });
}
}  // namespace detail
}  // namespace remote_command_runner
}  // namespace executor
}  // namespace mongo
