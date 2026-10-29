// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

namespace mongo {

class ReshardingMetricsCommon {
public:
    static constexpr size_t kRoleCount = 3;
    enum class Role { kCoordinator, kDonor, kRecipient };
    static std::string_view getRoleName(Role role);
};

}  // namespace mongo
