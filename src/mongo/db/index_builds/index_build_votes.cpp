// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_votes.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::index_builds::votes {

void runVoteCommand(OperationContext* opCtx,
                    ReplIndexBuildState& replState,
                    std::function<BSONObj(const UUID&, const std::string&)> generateCommand,
                    std::function<bool(const BSONObj&, const UUID&)> isDone) {

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // No locks should be held.
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    Backoff exponentialBackoff(Seconds(1), Seconds(2));

    auto onRemoteCmdScheduled = [opCtx, &replState](executor::TaskExecutor::CallbackHandle handle) {
        replState.onVoteRequestScheduled(opCtx, handle);
    };

    auto onRemoteCmdComplete = [&replState](executor::TaskExecutor::CallbackHandle) {
        replState.clearVoteRequestCbk();
    };

    auto needToVote = [&replState]() -> bool {
        return !replState.getNextActionNoWait();
    };

    // Retry command on error until we have been signaled either with commit or abort. This way, we
    // can make sure majority of nodes will never stop voting and wait for commit or abort signal
    // until they have received commit or abort signal.
    while (needToVote()) {
        // Check for any interrupts, including shutdown-related ones, before starting the voting
        // process.
        opCtx->checkForInterrupt();

        // Don't hammer the network.
        sleepFor(exponentialBackoff.nextSleep());
        // When index build started during startup recovery can try to get it's address when
        // rsConfig is uninitialized. So, retry till it gets initialized. Also, it's important, when
        // we retry, we check if we have received commit or abort signal to ensure liveness. For
        // e.g., consider a case where  index build gets restarted on startup recovery and
        // indexBuildsCoordinator thread waits for valid address w/o checking commit or abort
        // signal. Now, things can go wrong if we try to replay commitIndexBuild oplog entry for
        // that index build on startup recovery. Oplog applier would get stuck waiting on the
        // indexBuildsCoordinator thread. As a result, we won't be able to transition to secondary
        // state, get stuck on startup state.
        auto myAddress = replCoord->getMyHostAndPort();
        if (myAddress.empty()) {
            continue;
        }

        const BSONObj voteCmdRequest = generateCommand(replState.buildUUID, myAddress.toString());

        BSONObj voteCmdResponse;
        try {
            voteCmdResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(opCtx,
                                                                         DatabaseName::kAdmin,
                                                                         voteCmdRequest,
                                                                         onRemoteCmdScheduled,
                                                                         onRemoteCmdComplete);
        } catch (DBException& ex) {
            // All errors, including CallbackCanceled and network errors, should be retried.
            // If ErrorCodes::CallbackCanceled is due to shutdown, then checkForInterrupt() at the
            // beginning of this loop will catch it and throw an error to the caller. Or, if we
            // received the CallbackCanceled error because the index build was signaled with abort
            // or commit signal, then needToVote() would return false and we don't retry the voting
            // process.
            LOGV2_DEBUG(4666400,
                        1,
                        "Failed to run index build vote command.",
                        "command"_attr = voteCmdRequest.firstElement().fieldName(),
                        "indexBuildUUID"_attr = replState.buildUUID,
                        "errorMsg"_attr = ex);
            continue;
        }

        // Check if the command has to be retried.
        if (isDone(voteCmdResponse, replState.buildUUID)) {
            break;
        }
    }
}

AbortVoteResult runVoteAbortCommand(OperationContext* opCtx,
                                    ReplIndexBuildState& replState,
                                    std::string_view reason) {
    auto result = AbortVoteResult::kSignaled;

    const auto generateCmd = [reason](const UUID& uuid, const std::string& address) {
        return BSON("voteAbortIndexBuild" << uuid << "hostAndPort" << address << "reason" << reason
                                          << "writeConcern" << BSON("w" << "majority"));
    };

    const auto isDone = [&result](const BSONObj& response, const UUID& indexBuildUUID) {
        auto commandStatus = getStatusFromCommandResult(response);
        auto wcStatus = getWriteConcernStatusFromCommandResult(response);
        if (commandStatus.isOK() && wcStatus.isOK()) {
            result = AbortVoteResult::kAccepted;
            return true;
        }
        if (!commandStatus.isA<ErrorCategory::NotPrimaryError>()) {
            // If the index build failed to abort on the primary, retrying won't solve the issue.
            // This may only happen if the build cannot be aborted because it is being committed, or
            // because it is already aborted or committed, meaning an active index build is not
            // registered. In any of these cases, the primary will eventually replicate either a
            // 'commitIndexBuild' or 'abortIndexBuild' oplog entry. So it is safe to just wait for
            // the next action after the vote request is done.
            LOGV2(7329400,
                  "Index build: 'voteAbortIndexBuild' command failed, index build was not found or "
                  "is in the process of committing. The command won't be retried.",
                  "buildUUID"_attr = indexBuildUUID,
                  "responseStatus"_attr = response);
            // A write concern error alone still means the primary aborted the build.
            result = commandStatus.isOK() ? AbortVoteResult::kAccepted : AbortVoteResult::kRefused;
            return true;
        }

        // We should retry if the error is due to primary change.
        LOGV2(7329401,
              "Index build: 'voteAbortIndexBuild' command failed due to primary change and will be "
              "retried",
              "buildUUID"_attr = indexBuildUUID,
              "responseStatus"_attr = response);
        return false;
    };

    runVoteCommand(opCtx, replState, generateCmd, isDone);
    return result;
}

void runVoteCommitCommand(OperationContext* opCtx, ReplIndexBuildState& replState) {
    const auto generateCmd = [](const UUID& uuid, const std::string& address) {
        return BSON("voteCommitIndexBuild" << uuid << "hostAndPort" << address << "writeConcern"
                                           << BSON("w" << "majority"));
    };

    const auto isDone = [](const BSONObj& response, const UUID& indexBuildUUID) {
        // Command error and write concern error have to be retried.
        auto commandStatus = getStatusFromCommandResult(response);
        auto wcStatus = getWriteConcernStatusFromCommandResult(response);
        if (commandStatus.isOK() && wcStatus.isOK()) {
            return true;
        }
        LOGV2(3856202,
              "'voteCommitIndexBuild' command failed.",
              "indexBuildUUID"_attr = indexBuildUUID,
              "responseStatus"_attr = response);
        return false;
    };

    runVoteCommand(opCtx, replState, generateCmd, isDone);
}

}  // namespace mongo::index_builds::votes
