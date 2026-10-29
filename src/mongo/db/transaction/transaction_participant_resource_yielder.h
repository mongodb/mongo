// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/resource_yielder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo {

/**
 * Implementation of ResourceYielder that yields resources checked out in the course of running a
 * local replica set transaction.
 */
class [[MONGO_MOD_PUBLIC]] TransactionParticipantResourceYielder : public ResourceYielder {
public:
    TransactionParticipantResourceYielder(std::string_view cmdName) : _cmdName(cmdName) {
        invariant(!cmdName.empty());
    }

    /**
     * Returns a newly allocated yielder.
     */
    static std::unique_ptr<TransactionParticipantResourceYielder> make(std::string_view cmdName);

    /**
     * If the opCtx has a checked out Session, yields it and stashed its TransactionParticipant
     * resources.
     */
    void yield(OperationContext* opCtx) override;

    /**
     * If the opCtx had previously checked out a Session, checks it back out and unstashes its
     * TransactionParticipant resources. Note this may throw if the opCtx has been interrupted.
     */
    void unyield(OperationContext* opCtx) override;

    /**
     * Behaves the same as unyield, but does not throw. Instead, this function will ensure the
     * Session is checked in upon error, and will return a non-retryable error which wraps the
     * the original error because it's not safe to continue a transaction that failed to unyield.
     */
    Status unyieldNoThrow(OperationContext* opCtx) noexcept override;

private:
    bool _yielded = false;
    std::string _cmdName;
};

}  // namespace mongo
