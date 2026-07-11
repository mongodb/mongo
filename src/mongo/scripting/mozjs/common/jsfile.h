// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

struct [[MONGO_MOD_NEEDS_REPLACEMENT]] JSFile {
    const char* name;
    const std::string_view source;
};

}  // namespace mongo
