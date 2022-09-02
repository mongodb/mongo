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
#include "mongo/base/error_codes.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include <vector>

namespace mongo::executor::remote_command_runner {
namespace detail {
namespace {
const auto getRCRImpl = ServiceContext::declareDecoration<std::unique_ptr<RemoteCommandRunner>>();
}  // namespace

class RemoteCommandRunnerImpl : public RemoteCommandRunner {
public:
    /**
     * Executes the BSON command asynchronously on the given target.
     *
     * Do not call directly - this is not part of the public API.
     */
    ExecutorFuture<RemoteCommandInternalResponse> _doRequest(
        StringData dbName,
        BSONObj cmdBSON,
        std::unique_ptr<RemoteCommandHostTargeter> targeter,
        OperationContext* opCtx,
        std::shared_ptr<TaskExecutor> exec,
        CancellationToken token) final {
        return targeter->resolve(token)
            .thenRunOn(exec)
            .then([dbName, cmdBSON, opCtx, exec = std::move(exec), token](
                      std::vector<HostAndPort> targets) {
                uassert(ErrorCodes::HostNotFound, "No hosts availables", targets.size() != 0);

                RemoteCommandRequestOnAny executorRequest(
                    targets, dbName.toString(), cmdBSON, rpc::makeEmptyMetadata(), opCtx);
                return exec->scheduleRemoteCommandOnAny(executorRequest, token);
            })
            .then([&, exec = std::move(exec)](RemoteCommandOnAnyResponse r) {
                // Ensure the command didn't have a local error, or any remote errors, preferring
                // to propogate ok: 0 errors over writeConcern errors over write errors.
                iassert(r.status);
                iassert(getStatusFromCommandResult(r.data));
                iassert(getWriteConcernStatusFromCommandResult(r.data));
                iassert(getFirstWriteErrorStatusFromCommandResult(r.data));

                return RemoteCommandInternalResponse{r.data, r.target.get()};
            });
    }
};

const auto implRegisterer = ServiceContext::ConstructorActionRegisterer{
    "RemoteCommmandRunner",
    [](ServiceContext* ctx) { getRCRImpl(ctx) = std::make_unique<RemoteCommandRunnerImpl>(); }};

RemoteCommandRunner* RemoteCommandRunner::get(ServiceContext* svcCtx) {
    return getRCRImpl(svcCtx).get();
}

void RemoteCommandRunner::set(ServiceContext* svcCtx,
                              std::unique_ptr<RemoteCommandRunner> theRunner) {
    getRCRImpl(svcCtx) = std::move(theRunner);
}
}  // namespace detail
}  // namespace mongo::executor::remote_command_runner
