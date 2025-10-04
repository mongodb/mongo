/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * Implementation of ResourceYielder that yields resources checked out in the course of running a
 * local replica set transaction.
 */
class TransactionParticipantResourceYielder : public ResourceYielder {
public:
    TransactionParticipantResourceYielder(StringData cmdName) : _cmdName(cmdName) {
        invariant(!cmdName.empty());
    }

    /**
     * Returns a newly allocated yielder.
     */
    static std::unique_ptr<TransactionParticipantResourceYielder> make(StringData cmdName);

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
