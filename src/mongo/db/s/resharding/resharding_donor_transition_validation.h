// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/resharding_transition_check.h"
#include "mongo/s/resharding/common_types_gen.h"

namespace mongo {

/**
 * Runtime state the donor state machine gathers for transition checks.
 */
struct DonorTransitionFacts {
    bool performVerification = false;
    bool allRecipientsDoneCloning = false;
    bool allRecipientsDoneApplying = false;
    bool changeStreamsMonitorStarted = false;
    bool inDonatingOplogEntries = false;   // post only
    bool inBlockingWritesOrError = false;  // post only
};

struct DonorPreTransitionInput {
    const DonorShardContext& before;
    const DonorShardContext& proposed;
    DonorTransitionFacts facts;
};

struct DonorPostTransitionInput {
    const DonorShardContext& before;
    const DonorShardContext& proposed;
    const DonorShardContext& durable;
    DonorTransitionFacts facts;
};

ConstraintResult validateDonorPreTransition(const DonorPreTransitionInput& input);

ConstraintResult validateDonorPostTransition(const DonorPostTransitionInput& input);

}  // namespace mongo
