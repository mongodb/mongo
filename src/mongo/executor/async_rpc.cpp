// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/async_rpc.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/database_name_util.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

// TODO(SERVER-98556): kTest debug statements for the purpose of helping with diagnosing BFs.
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::async_rpc {
namespace detail {
namespace {
const auto getRCRImpl = ServiceContext::declareDecoration<std::unique_ptr<AsyncRPCRunner>>();
}  // namespace

MONGO_FAIL_POINT_DEFINE(pauseAsyncRPCAfterNetworkResponse);
MONGO_FAIL_POINT_DEFINE(pauseScheduleCallWithCancelTokenUntilCanceled);

class AsyncRPCRunnerImpl : public AsyncRPCRunner {
public:
    /**
     * Executes the BSON command asynchronously on the given target.
     *
     * Do not call directly - this is not part of the public API.
     */
    ExecutorFuture<AsyncRPCInternalResponse> _sendCommand(
        std::shared_ptr<TaskExecutor> exec,
        CancellationToken token,
        OperationContext* opCtx,
        Targeter* targeter,
        const TargetingMetadata& targetingMetadata,
        const DatabaseName& dbName,
        BSONObj cmdBSON,
        BatonHandle baton,
        boost::optional<UUID> clientOperationKey) final {
        auto proxyExec = std::make_shared<ProxyingExecutor>(exec, baton);
        return targeter->resolve(token, targetingMetadata)
            .thenRunOn(proxyExec)
            .then([dbName,
                   cmdBSON,
                   opCtx,
                   exec = std::move(exec),
                   token,
                   baton = std::move(baton),
                   clientOperationKey](const HostAndPort& target) {
                executor::RemoteCommandRequest executorRequest(
                    target,
                    dbName,
                    cmdBSON,
                    rpc::makeEmptyMetadata(),
                    opCtx,
                    executor::RemoteCommandRequest::kNoTimeout,
                    {},
                    clientOperationKey);

                // Fail point to make this method to wait until the token is canceled.
                if (!token.isCanceled()) {
                    try {
                        pauseScheduleCallWithCancelTokenUntilCanceled.pauseWhileSetAndNotCanceled(
                            Interruptible::notInterruptible(), token);
                    } catch (ExceptionFor<ErrorCodes::Interrupted>&) {
                        // Swallow the interrupted exception that arrives from canceling a
                        // failpoint.
                    }
                }

                auto [p, f] = makePromiseFuture<TaskExecutor::RemoteCommandCallbackArgs>();
                auto swCallbackHandle = exec->scheduleRemoteCommand(
                    executorRequest,
                    [p = std::make_shared<Promise<TaskExecutor::RemoteCommandCallbackArgs>>(
                         std::move(p))](const TaskExecutor::RemoteCommandCallbackArgs& cbData) {
                        pauseAsyncRPCAfterNetworkResponse.pauseWhileSet();
                        p->emplaceValue(cbData);
                    },
                    std::move(baton));
                uassertStatusOK(swCallbackHandle);
                token.onCancel()
                    .unsafeToInlineFuture()
                    .then(
                        [exec, callbackHandle = std::move(swCallbackHandle.getValue())]() mutable {
                            // TODO(SERVER-98556): Debug statement for the purpose of helping with
                            // diagnosing BFs.
                            LOGV2_DEBUG(9771000, 1, "Cancellation recognized in async_rpc");
                            exec->cancel(callbackHandle);
                        })
                    .getAsync([](auto) {});
                return std::move(f);
            })
            .onError(
                [](Status s) -> StatusWith<TaskExecutor::TaskExecutor::RemoteCommandCallbackArgs> {
                    // If there was a scheduling error or other local error before the
                    // command was accepted by the executor.
                    return Status{AsyncRPCErrorInfo(s, {}), "Remote command execution failed"};
                })
            .then([targeter](TaskExecutor::RemoteCommandCallbackArgs cbargs) {
                auto r = cbargs.response;
                auto s = makeErrorIfNeeded(r);
                // Update targeter for errors.
                if (!s.isOK() && s.code() == ErrorCodes::RemoteCommandExecutionError) {
                    auto extraInfo = s.extraInfo<AsyncRPCErrorInfo>();
                    if (extraInfo->isLocal()) {
                        targeter->onRemoteCommandError(r.target, extraInfo->asLocal()).get();
                    } else {
                        // TODO(SERVER-98556): Debug statement for the purpose of helping with
                        // diagnosing BFs.
                        LOGV2_DEBUG(9771003,
                                    1,
                                    "Remote error occured in async_rpc",
                                    "status"_attr =
                                        extraInfo->asRemote().getRemoteCommandResult().toString());
                        targeter
                            ->onRemoteCommandError(r.target,
                                                   extraInfo->asRemote().getRemoteCommandResult())
                            .get();
                    }
                }
                uassertStatusOK(s);
                return AsyncRPCInternalResponse{r.data, r.target, *r.elapsed};
            });
    }
};

const auto implRegisterer =
    ServiceContext::ConstructorActionRegisterer{"RemoteCommmandRunner", [](ServiceContext* ctx) {
                                                    getRCRImpl(ctx) =
                                                        std::make_unique<AsyncRPCRunnerImpl>();
                                                }};

AsyncRPCRunner* AsyncRPCRunner::get(ServiceContext* svcCtx) {
    return getRCRImpl(svcCtx).get();
}

void AsyncRPCRunner::set(ServiceContext* svcCtx, std::unique_ptr<AsyncRPCRunner> theRunner) {
    getRCRImpl(svcCtx) = std::move(theRunner);
}
}  // namespace detail
}  // namespace mongo::async_rpc
