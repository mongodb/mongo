/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/api_parameters.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/generic_argument_gen.h"

namespace mongo::generic_argument_util {

/**
 * Generic majority WC to use for intra-cluster communication that requires the recipient to execute
 * some command with a majority WC. This is used by `setMajorityWriteConcern` and
 * `CommandHelpers::appendMajorityWriteConcern` unless another is specified.
 */
extern const WriteConcernOptions kMajorityWriteConcern;

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
 * generic_argument_util::kMajorityWriteConcern.
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
    } else if (defaultWC && !defaultWC->usedDefaultConstructedWC) {
        auto wc = *defaultWC;
        wc.w = WriteConcernOptions::kMajority;
        if (wc.wTimeout < kMajorityWriteConcern.wTimeout) {
            wc.wTimeout = kMajorityWriteConcern.wTimeout;
        }
        args.setWriteConcern(std::move(wc));
    } else {
        args.setWriteConcern(kMajorityWriteConcern);
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

}  // namespace mongo::generic_argument_util
