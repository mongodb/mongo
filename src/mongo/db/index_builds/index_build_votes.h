// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/repl_index_build_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <string>
#include <string_view>

/**
 * Sends a two-phase index build's commit and abort votes to the primary.
 */
[[MONGO_MOD_PUBLIC]];
namespace mongo::index_builds::votes {

enum class AbortVoteResult {
    // The primary aborted the build.
    kAccepted,
    // The primary refused, because it is committing the build or no longer running it.
    kRefused,
    // The build got a commit or abort signal before the primary answered.
    kSignaled,
};

/**
 * Sends the command from 'generateCommand' to the primary until 'isDone' accepts a response or the
 * build gets a commit or abort signal. Call without holding locks.
 */
void runVoteCommand(OperationContext* opCtx,
                    ReplIndexBuildState& replState,
                    std::function<BSONObj(const UUID&, const std::string&)> generateCommand,
                    std::function<bool(const BSONObj&, const UUID&)> isDone);

/**
 * Sends 'voteAbortIndexBuild' to the primary with write concern 'majority'. Retries after primary
 * changes until the primary answers or the build gets a signal.
 */
AbortVoteResult runVoteAbortCommand(OperationContext* opCtx,
                                    ReplIndexBuildState& replState,
                                    std::string_view reason);

/**
 * Sends 'voteCommitIndexBuild' to the primary with write concern 'majority'. Retries any error
 * until the primary accepts or the build gets a signal.
 */
void runVoteCommitCommand(OperationContext* opCtx, ReplIndexBuildState& replState);

}  // namespace mongo::index_builds::votes
