// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Returns a settable optional indicating whether the multi-document transaction the
 * TransactionParticipant checked out on this operation context was initiated by the server (an
 * internal transaction) rather than by a user. Empty if no participant has begun or continued a
 * transaction on this operation context. Set by TransactionParticipant::beginOrContinue on every
 * statement, since each statement gets a fresh operation context.
 *
 * A shard acting as a sub-router needs this to classify the transaction commands it forwards to
 * new participants.
 */
boost::optional<bool>& participantIsInServerInitiatedTransaction(OperationContext*);

}  // namespace mongo
