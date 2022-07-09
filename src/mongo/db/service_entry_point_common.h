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

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/message.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/polymorphic_scoped.h"

namespace mongo {

extern FailPoint respondWithNotPrimaryInCommandDispatch;

// When active, we won't check if we are primary in command dispatch. Activate this if you want to
// test failing during command execution.
extern FailPoint skipCheckingForNotPrimaryInCommandDispatch;

/**
 * Helpers for writing ServiceEntryPointImpl implementations from a reusable core.
 * Implementations are ServiceEntryPointMongod and ServiceEntryPointEmbedded, which share
 * most of their code, but vary in small details captured by the Hooks customization
 * interface.
 */
struct ServiceEntryPointCommon {
    /**
     * Interface for customizing ServiceEntryPointImpl behavior.
     */
    class Hooks {
    public:
        virtual ~Hooks();
        virtual bool lockedForWriting() const = 0;
        virtual void setPrepareConflictBehaviorForReadConcern(
            OperationContext* opCtx, const CommandInvocation* invocation) const = 0;
        virtual void waitForReadConcern(OperationContext* opCtx,
                                        const CommandInvocation* invocation,
                                        const OpMsgRequest& request) const = 0;
        /**
         * Waits to satisfy a speculative majority read, if necessary.
         *
         * Speculative reads block after a query has executed to ensure that any data read satisfies
         * the appropriate durability properties e.g. "majority" read concern. If the operation is
         * not a speculative read, then this method does nothing.
         */
        virtual void waitForSpeculativeMajorityReadConcern(OperationContext* opCtx) const = 0;
        virtual void waitForWriteConcern(OperationContext* opCtx,
                                         const CommandInvocation* invocation,
                                         const repl::OpTime& lastOpBeforeRun,
                                         BSONObjBuilder& commandResponseBuilder) const = 0;

        virtual void waitForLinearizableReadConcern(OperationContext* opCtx) const = 0;
        virtual void uassertCommandDoesNotSpecifyWriteConcern(const BSONObj& cmdObj) const = 0;

        virtual void attachCurOpErrInfo(OperationContext* opCtx, const BSONObj& replyObj) const = 0;

        virtual bool refreshDatabase(OperationContext* opCtx, const StaleDbRoutingVersion& se) const
            noexcept = 0;

        virtual bool refreshCollection(OperationContext* opCtx, const StaleConfigInfo& se) const
            noexcept = 0;

        virtual bool refreshCatalogCache(
            OperationContext* opCtx, const ShardCannotRefreshDueToLocksHeldInfo& refreshInfo) const
            noexcept = 0;

        virtual void handleReshardingCriticalSectionMetrics(OperationContext* opCtx,
                                                            const StaleConfigInfo& se) const
            noexcept = 0;

        virtual void resetLockerState(OperationContext* opCtx) const noexcept = 0;

        MONGO_WARN_UNUSED_RESULT_FUNCTION virtual std::unique_ptr<PolymorphicScoped>
        scopedOperationCompletionShardingActions(OperationContext* opCtx) const = 0;

        virtual void appendReplyMetadata(OperationContext* opCtx,
                                         const OpMsgRequest& request,
                                         BSONObjBuilder* metadataBob) const = 0;
    };

    static Future<DbResponse> handleRequest(OperationContext* opCtx,
                                            const Message& m,
                                            std::unique_ptr<const Hooks> hooks) noexcept;

    /**
     * Produce a new object based on cmdObj, but with redactions applied as specified by
     * `command->redactForLogging`.
     */
    static BSONObj getRedactedCopyForLogging(const Command* command, const BSONObj& cmdObj);
};

}  // namespace mongo
