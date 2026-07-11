// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/action_type_gen.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * List describing the ActionTypes that should be created.
 * Please note that the order of the elements is not guaranteed to be the same across versions.
 * This means that the integer value assigned to each ActionType and used internally in ActionSet
 * also may change between versions.
 */
using ActionType = ActionTypeEnum;
constexpr inline size_t kNumActionTypes = idlEnumCount<ActionTypeEnum>;

StatusWith<ActionType> parseActionFromString(std::string_view action);
std::string_view toStringData(ActionType a);
std::string toString(ActionType a);
std::ostream& operator<<(std::ostream& os, const ActionType& a);

}  // namespace mongo
