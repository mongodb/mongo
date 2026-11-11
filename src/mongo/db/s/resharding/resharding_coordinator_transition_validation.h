// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_transition_check.h"

namespace mongo {

/**
 * In-memory state the coordinator knew when it decided to transition, not persisted in the
 * coordinator document.
 */
struct CoordinatorTransitionFacts {
    bool canEnterCritical = false;
};

struct CoordinatorPreTransitionInput {
    const ReshardingCoordinatorDocument& before;
    const ReshardingCoordinatorDocument& proposed;
    CoordinatorTransitionFacts facts;
};

ConstraintResult validateCoordinatorPreTransition(const CoordinatorPreTransitionInput& input);

}  // namespace mongo
