// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo {
using namespace std::literals::string_view_literals;

inline Status validateShardingClusterRoleSetting(const std::string& value) {
    if (!(str::equalCaseInsensitive(value, "configsvr"sv) ||
          str::equalCaseInsensitive(value, "shardsvr"sv))) {
        return {ErrorCodes::BadValue,
                "sharding.clusterRole must be one of 'configsvr' or 'shardsvr'"};
    }

    return Status::OK();
}

inline Status validateMaintenanceModeSetting(const std::string& value) {
    constexpr auto kReplicaSet = "replicaSet"sv;
    constexpr auto kStandalone = "standalone"sv;
    if (kStandalone != value && kReplicaSet != value) {
        return {ErrorCodes::BadValue,
                "maintenanceMode must be one of 'replicaSet' or 'standalone'"};
    }

    return Status::OK();
}

}  // namespace mongo
