// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/errno_util.h"

#include <string_view>
#include <system_error>

#include <fmt/format.h>

namespace mongo {
inline std::string getLastSystemErrorMessageFormatted(std::string_view op,
                                                      const std::string& path) {
    std::error_code ec = lastSystemError();
    return fmt::format(
        "Failed to {} {}: error code = {}, {}", op, path, ec.value(), errorMessage(ec));
}
}  // namespace mongo
