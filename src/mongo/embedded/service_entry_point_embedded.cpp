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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/embedded/service_entry_point_embedded.h"

#include "mongo/db/read_concern.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/embedded/not_implemented.h"
#include "mongo/embedded/periodic_runner_embedded.h"

namespace mongo {

class ServiceEntryPointEmbedded::Hooks final : public ServiceEntryPointCommon::Hooks {
public:
    bool lockedForWriting() const override {
        return false;
    }

    void waitForReadConcern(OperationContext* opCtx,
                            const CommandInvocation* invocation,
                            const OpMsgRequest& request) const override {
        const auto prepareConflictBehavior = invocation->canIgnorePrepareConflicts()
            ? PrepareConflictBehavior::kIgnore
            : PrepareConflictBehavior::kEnforce;
        auto rcStatus = mongo::waitForReadConcern(opCtx,
                                                  repl::ReadConcernArgs::get(opCtx),
                                                  invocation->allowsAfterClusterTime(),
                                                  prepareConflictBehavior);
        uassertStatusOK(rcStatus);
    }

    void waitForSpeculativeMajorityReadConcern(OperationContext* opCtx) const override {
        auto rcStatus = mongo::waitForSpeculativeMajorityReadConcern(
            opCtx, repl::SpeculativeMajorityReadInfo::get(opCtx));
        uassertStatusOK(rcStatus);
    }

    void waitForWriteConcern(OperationContext* opCtx,
                             const CommandInvocation* invocation,
                             const repl::OpTime& lastOpBeforeRun,
                             BSONObjBuilder& commandResponseBuilder) const override {
        WriteConcernResult res;
        auto waitForWCStatus =
            mongo::waitForWriteConcern(opCtx, lastOpBeforeRun, opCtx->getWriteConcern(), &res);

        CommandHelpers::appendCommandWCStatus(commandResponseBuilder, waitForWCStatus, res);
    }

    void waitForLinearizableReadConcern(OperationContext* opCtx) const override {
        if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
            repl::ReadConcernLevel::kLinearizableReadConcern) {
            uassertStatusOK(mongo::waitForLinearizableReadConcern(opCtx, 0));
        }
    }

    void uassertCommandDoesNotSpecifyWriteConcern(const BSONObj& cmd) const override {
        if (commandSpecifiesWriteConcern(cmd)) {
            uasserted(ErrorCodes::InvalidOptions, "Command does not support writeConcern");
        }
    }

    void attachCurOpErrInfo(OperationContext*, const BSONObj&) const override {}

    void handleException(const DBException& e, OperationContext* opCtx) const override {}

    void advanceConfigOptimeFromRequestMetadata(OperationContext* opCtx) const override {}

    std::unique_ptr<PolymorphicScoped> scopedOperationCompletionShardingActions(
        OperationContext* opCtx) const override {
        return nullptr;
    }

    void appendReplyMetadataOnError(OperationContext* opCtx,
                                    BSONObjBuilder* metadataBob) const override {}

    void appendReplyMetadata(OperationContext* opCtx,
                             const OpMsgRequest& request,
                             BSONObjBuilder* metadataBob) const override {}
};

DbResponse ServiceEntryPointEmbedded::handleRequest(OperationContext* opCtx, const Message& m) {
    // Only one thread will pump at a time and concurrent calls to this will skip the pumping and go
    // directly to handleRequest. This means that the jobs in the periodic runner can't provide any
    // guarantees of the state (that they have run).
    checked_cast<PeriodicRunnerEmbedded*>(opCtx->getServiceContext()->getPeriodicRunner())
        ->tryPump();
    return ServiceEntryPointCommon::handleRequest(opCtx, m, Hooks{});
}

void ServiceEntryPointEmbedded::startSession(transport::SessionHandle session) {
    UASSERT_NOT_IMPLEMENTED;
}

void ServiceEntryPointEmbedded::endAllSessions(transport::Session::TagMask tags) {}

Status ServiceEntryPointEmbedded::start() {
    UASSERT_NOT_IMPLEMENTED;
}

bool ServiceEntryPointEmbedded::shutdown(Milliseconds timeout) {
    UASSERT_NOT_IMPLEMENTED;
}

void ServiceEntryPointEmbedded::appendStats(BSONObjBuilder*) const {
    UASSERT_NOT_IMPLEMENTED;
}

size_t ServiceEntryPointEmbedded::numOpenSessions() const {
    UASSERT_NOT_IMPLEMENTED;
}

}  // namespace mongo
