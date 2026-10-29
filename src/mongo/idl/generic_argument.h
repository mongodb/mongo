// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]];

namespace mongo {

/**
 * Utility functions used by generic_argument.idl which are included in anything that includes it.
 * Therefore, this cannot include generic_argument_gen.h
 */

/**
 * 'maxTimeMSOpOnly' values are allowed to exceed the max allowed value for 'maxTimeMS' by a small
 * constant (100). This is because mongod and mongos server processes add a small amount to the
 * 'maxTimeMS' value they are given before passing it on as 'maxTimeMSOpOnly', to allow for clock
 * precision.
 */
Status validateMaxTimeMSOpOnly(std::int64_t val);

}  // namespace mongo
