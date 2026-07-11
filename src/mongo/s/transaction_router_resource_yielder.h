// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/resource_yielder.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * Implementation of ResourceYielder that yields resources checked out in the course of running a
 * distributed transaction.
 */
class [[MONGO_MOD_PUBLIC]] TransactionRouterResourceYielder : public ResourceYielder {
public:
    /**
     * The next two methods return a newly allocated yielder for the given yielding scenario.
     *
     * Use makeForLocalHandoff() when the operation expected to check out the session next is in the
     * same local process, e.g. a mongos command in a transaction is yielding so a command spawned
     * by the transaction API can check out the same session.
     *
     * Use makeForRemoteCommand() when yielding waiting for a possibly remote response to be
     * received. If the local process serves both a router and mongod role, the local process may
     * need to check out the same session to produce the response.
     *
     * The only difference in behavior is that makeForLocalHandoff() always returns a yielder and
     * makeForRemoteCommand() returns nullptr if the local process is a mongos because a mongos
     * cannot target itself.
     */
    static std::unique_ptr<TransactionRouterResourceYielder> makeForLocalHandoff();
    static std::unique_ptr<TransactionRouterResourceYielder> makeForRemoteCommand();

    /**
     * If the opCtx has a checked out RouterOperationContextSession, yields it.
     */
    void yield(OperationContext* opCtx) override;

    /**
     * If the opCtx had previously checked out RouterOperationContextSession, checks it back out.
     * Note this may throw if the opCtx is interrupted at global shutdown. Otherwise it should never
     * fail.
     */
    void unyield(OperationContext* opCtx) override;

private:
    bool _yielded{false};
};

}  // namespace mongo
