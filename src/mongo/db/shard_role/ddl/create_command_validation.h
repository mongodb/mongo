// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

namespace mongo::create_command_validation {

[[MONGO_MOD_PARENT_PRIVATE]] inline Status validateViewOnNotEmpty(const std::string& viewOn) {
    return viewOn.empty()
        ? Status(ErrorCodes::BadValue, str::stream() << "'viewOn' cannot be empty")
        : Status::OK();
}

}  // namespace mongo::create_command_validation
