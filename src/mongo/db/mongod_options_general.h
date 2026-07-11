// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

namespace mongo {

inline Status validateSecurityAuthorizationSetting(const std::string& value) {
    using namespace std::literals::string_view_literals;
    if (!(str::equalCaseInsensitive(value, "enabled"sv) ||
          str::equalCaseInsensitive(value, "disabled"sv))) {
        return {ErrorCodes::BadValue,
                "security.authorization expects either 'enabled' or 'disabled'"};
    }

    return Status::OK();
}

inline Status validateOperationProfilingModeSetting(const std::string& value) {
    using namespace std::literals::string_view_literals;
    if (!(str::equalCaseInsensitive(value, "off"sv) ||
          str::equalCaseInsensitive(value, "slowOp"sv) ||
          str::equalCaseInsensitive(value, "all"sv))) {
        return {ErrorCodes::BadValue, "operationProfiling.mode expects 'off', 'slowOp', or 'all'"};
    }

    return Status::OK();
}

}  // namespace mongo
