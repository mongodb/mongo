// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
/**
 * This class allows for the storageEngine to alert the rest of the system about journaled write
 * progress.
 *
 * It has two methods. The first, getToken(), returns a token representing the current progress
 * applied to the node. It should be called just prior to making writes durable (usually, syncing a
 * journal entry to disk).
 *
 * The second method, onDurable(), takes this token as an argument and relays to the rest of the
 * system that writes through that point have been journaled.
 */
class [[MONGO_MOD_OPEN]] JournalListener {
public:
    class [[MONGO_MOD_OPEN]] Token {
    public:
        virtual ~Token() = default;
    };
    virtual ~JournalListener() = default;
    virtual std::unique_ptr<Token> getToken(OperationContext* opCtx) = 0;
    virtual void onDurable(const Token& token) = 0;
};

}  // namespace mongo
