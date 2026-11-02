// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/resharding/resharding_tunables_gen.h"

namespace mongo {
namespace resharding {

/**
 * Sets every tunable that is still unset to the current value of the corresponding resharding
 * server parameter on this node. Tunables that were explicitly specified are left untouched.
 *
 * This is called once, by the resharding coordinator on the config server, when an operation
 * starts. The resulting complete set is persisted and sent to the participants, so that the
 * server parameters of the individual nodes no longer influence the operation.
 */
void fillDefaults(ReshardingTunables& tunables);

}  // namespace resharding
}  // namespace mongo
