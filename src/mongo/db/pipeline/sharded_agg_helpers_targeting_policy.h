// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {
enum class [[MONGO_MOD_PUBLIC]] ShardTargetingPolicy {
    kNotAllowed,
    kAllowed,
    kForceTargetingWithSimpleCollation,
};
}  // namespace mongo
