// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

inline Status validateDatabaseName(const std::string& value) {
    if (value.empty()) {
        return {ErrorCodes::NoSuchKey, "Database name cannot be empty"};
    }
    return Status::OK();
}

inline Status validateDatabaseName(const DatabaseName& value) {
    if (value.isEmpty()) {
        return {ErrorCodes::NoSuchKey, "Database name cannot be empty"};
    }
    return Status::OK();
}

}  // namespace mongo
