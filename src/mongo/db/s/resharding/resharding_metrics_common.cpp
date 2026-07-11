// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_metrics_common.h"

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
const stdx::unordered_map<ReshardingMetricsCommon::Role, std::string_view> roleToName = {
    {ReshardingMetricsCommon::Role::kCoordinator, "Coordinator"sv},
    {ReshardingMetricsCommon::Role::kDonor, "Donor"sv},
    {ReshardingMetricsCommon::Role::kRecipient, "Recipient"sv},
};
}  // namespace

std::string_view ReshardingMetricsCommon::getRoleName(Role role) {
    auto it = roleToName.find(role);
    invariant(it != roleToName.end());
    return it->second;
}

}  // namespace mongo
