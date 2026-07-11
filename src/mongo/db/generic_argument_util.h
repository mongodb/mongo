// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/api_parameters.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Hardcoded majority write concern with no timeout to use for intra-cluster writes.
 *
 * NOTE: Callers that need to wait for majority write concern must consider at least the following
 * implications:
 *
 * (1) The wait can last a very long time (in case of replication lag). Depending on
 * how the waiters are scheduled, this can cause a build-up of threads blocked on majority
 * replication, so the caller needs to ensure that this does not impact availability (OOM the
 * server). Because of (2) below, adding a timeout is usually not a good solution.
 *
 * (2) The wait can eventually fail, which means the state of the write is indeterminate, so the
 * caller needs to be prepared to recover. In most cases, this recovery means using retryable writes
 * or transactions; however if this is not possible another, more complicated recovery protocol may
 * need to be implemented.
 */
WriteConcernOptions defaultMajorityWriteConcern();

/**
 * Hardcoded majority write concern to use for intra-cluster writes, that requires the wait for
 * majority to fail if not satisfied within a fixed time, because the caller is not prepared for a
 * longer wait.
 *
 * The "fixed time" is arbitrarily selected to be 60 seconds and there could be tests and possibly
 * external features that rely on this value, so it must not be changed. Instead all users of this
 * value must eventually be converted to the `defaultMajorityWriteConcern` variant above.
 *
 * DO NOT add any new usages of this function and instead use the `defaultMajorityWriteConcern`
 * variant above and carefully read its comments.
 */
WriteConcernOptions defaultMajorityWriteConcernDoNotUse();

namespace generic_argument_util {

/**
 * Command helpers that operate on an idl struct rather than modifying an existing BSONObj.
 */

/**
 * Sets a majority write concern on the IDL-generated command or GenericArguments struct.
 *
 * If the command already contains a write concern, this sets its "w" field to "majority" and leaves
 * the other write concern options as-is.
 *
 * If the comand does not contain a write concern and `defaultWC` was provided, `defaultWC` will be
 * set. Its "w" field will be set to "majority" if it was not set to that already.
 *
 * In all other cases, the write concern will be set to
 * defaultMajorityWriteConcernDoNotUse().
 */
template <typename CommandType>
void setMajorityWriteConcern(CommandType& args, const WriteConcernOptions* defaultWC = nullptr) {
    if (auto& parsedWC = args.getWriteConcern()) {
        // The command has a writeConcern field and it's majority, so we can return it as-is.
        if (parsedWC->isMajority()) {
            return;
        }

        auto newWC = parsedWC;
        newWC->w = WriteConcernOptions::kMajority;
        args.setWriteConcern(std::move(newWC));
    }

    auto&& global = defaultMajorityWriteConcernDoNotUse();
    if (defaultWC && !defaultWC->usedDefaultConstructedWC) {
        auto wc = *defaultWC;
        wc.w = WriteConcernOptions::kMajority;
        wc.wTimeout = std::max(wc.wTimeout, global.wTimeout);
        args.setWriteConcern(std::move(wc));
    } else {
        args.setWriteConcern(global);
    }
}

inline LogicalSessionFromClient toLogicalSessionFromClient(const LogicalSessionId& lsid) {
    LogicalSessionFromClient lsidfc;
    lsidfc.setId(lsid.getId());
    lsidfc.setUid(lsid.getUid());
    lsidfc.setTxnUUID(lsid.getTxnUUID());
    lsidfc.setTxnNumber(lsid.getTxnNumber());
    return lsidfc;
}

inline OperationSessionInfoFromClient getOperationSessionInfoFromClient(
    const GenericArguments& args) {
    OperationSessionInfoFromClient osi;
    osi.setSessionId(args.getLsid());
    osi.setTxnNumber(args.getTxnNumber());
    osi.setTxnRetryCounter(args.getTxnRetryCounter());
    osi.setAutocommit(args.getAutocommit());
    osi.setStartTransaction(args.getStartTransaction());
    osi.setStartOrContinueTransaction(args.getStartOrContinueTransaction());
    osi.setCoordinator(args.getCoordinator());
    osi.setTransactionRuntimeContext(args.getTransactionRuntimeContext());
    return osi;
}

template <typename CommandType>
void setOperationSessionInfo(CommandType& request, const OperationSessionInfo& osi) {
    if (auto sid = osi.getSessionId()) {
        request.setLsid(toLogicalSessionFromClient(*sid));
    } else {
        request.setLsid(boost::none);
    }
    request.setTxnNumber(osi.getTxnNumber());
    request.setTxnRetryCounter(osi.getTxnRetryCounter());
    request.setAutocommit(osi.getAutocommit());
    request.setStartTransaction(osi.getStartTransaction());
    request.setStartOrContinueTransaction(osi.getStartOrContinueTransaction());
}

template <typename CommandType>
void setAPIParameters(CommandType& request, const APIParameters& params) {
    params.setInfo(request);
}

template <typename CommandType>
void setDbVersionIfPresent(CommandType& cmd, DatabaseVersion dbVersion) {
    if (!dbVersion.isFixed()) {
        cmd.setDatabaseVersion(dbVersion);
    }
}

/**
 * Strip generic arguments that will later be appended by the internal transaction machinery.
 * This is essentially the inverse of Transaction::prepareRequest.
 */
template <typename CommandType>
void prepareRequestForInternalTransactionPassthrough(CommandType& cmd) {
    cmd.setWriteConcern(boost::none);
    cmd.setReadConcern(boost::none);
    cmd.setMaxTimeMS(boost::none);
    setOperationSessionInfo(cmd, {});
    setAPIParameters(cmd, {});
}

/**
 * Strip generic arguments that mongot does not recognize.
 *
 * mongot doesn't currently recognize any generic arguments, including shard-forwardable generic
 * arguments (e.g. lsid), so this just drops them all.
 */
template <typename CommandType>
void prepareRequestForSearchIndexManagerPassthrough(CommandType& cmd) {
    cmd.setGenericArguments({});
}

}  // namespace generic_argument_util
}  // namespace mongo
