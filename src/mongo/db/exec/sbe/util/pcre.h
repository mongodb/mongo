// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::sbe {
value::TagValueOwned makeNewPcreRegex(std::string_view pattern, std::string_view options);
}  // namespace mongo::sbe
