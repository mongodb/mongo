// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

inline Status validateReplicaSetNameSetting(const std::string& value) {
    if (value.find('/') != std::string::npos) {
        return {ErrorCodes::BadValue, "replication.replSetName must not contain a '/' character"};
    }

    return Status::OK();
}

}  // namespace mongo
