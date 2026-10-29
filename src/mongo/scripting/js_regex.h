// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

struct JSRegEx {
    std::string pattern;
    std::string flags;

    JSRegEx() = default;
    JSRegEx(std::string pattern, std::string flags)
        : pattern(std::move(pattern)), flags(std::move(flags)) {}
};

}  // namespace mongo
