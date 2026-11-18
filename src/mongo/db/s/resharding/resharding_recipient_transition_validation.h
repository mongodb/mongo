// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/s/resharding/resharding_transition_check.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/time_support.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Contracts for the recipient's state transitions. Every legal edge (from, to) has a validator that
 * reports each rule that does not hold. An edge not in the table is itself a violation
 * ("isAllowedEdge"), so a new state or edge needs a contract. Rules are TransitionConstraints
 * chained through TransitionCheck (resharding_transition_check.h), as in the donor's contracts.
 */

/**
 * The fields of the recipient document that the contracts examine. The clone details are stored on
 * the document, not in RecipientShardContext.
 */
struct RecipientTransitionSnapshot {
    RecipientShardContext ctx;
    boost::optional<Timestamp> cloneTimestamp;
    boost::optional<int64_t> approxDocumentsToCopy;
    boost::optional<int64_t> approxBytesToCopy;
    boost::optional<Date_t> startConfigTxnCloneTime;

    // Includes each donor's minFetchTimestamp. Fixed at initialization, except for those
    // timestamps, which the edge validators govern.
    ReshardingRecipientOptions options;

    // Fixed for the operation. Lives in the shared CommonReshardingMetadata, not the options.
    bool performVerification{false};
};

struct RecipientPreTransitionInput {
    const RecipientTransitionSnapshot& before;
    const RecipientTransitionSnapshot& proposed;
    // The state the state machine is leaving, as it believes (default: the persisted one). A
    // mismatch means memory and disk diverged.
    boost::optional<RecipientStateEnum> outgoingPhase;
};

// What the post-transition check compares: what was proposed, against what the in-memory context
// and the persisted document hold after the write.
struct RecipientPostTransitionInput {
    const RecipientTransitionSnapshot& proposed;
    const RecipientShardContext& inMemory;
    const RecipientTransitionSnapshot& durable;
};

/**
 * Checks, before the write, that the recipient may move from 'before' to 'proposed'.
 */
ConstraintResult validateRecipientPreTransition(const RecipientPreTransitionInput& input);

/** Checks, after the write, that the document and the in-memory context hold the proposed state. */
ConstraintResult validateRecipientPostTransition(const RecipientPostTransitionInput& input);

}  // namespace mongo
