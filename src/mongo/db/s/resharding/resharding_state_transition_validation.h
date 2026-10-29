// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/resharding_transition_check.h"
#include "mongo/s/resharding/common_types_gen.h"

namespace mongo {

struct DonorPreTransitionInput {
    const DonorShardContext& before;
    const DonorShardContext& proposed;
};

struct DonorPostTransitionInput {
    const DonorShardContext& proposed;
    const DonorShardContext& inMemory;
    const DonorShardContext& durable;
};

ConstraintResult validateDonorPreTransition(const DonorPreTransitionInput& input);

ConstraintResult validateDonorPostTransition(const DonorPostTransitionInput& input);

}  // namespace mongo
