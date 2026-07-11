// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

void validateRseqKernelCompat();
[[MONGO_MOD_FILE_PRIVATE]] bool isKernelVersionSafeForTCMallocPerCPUCache(std::string_view release);

}  // namespace mongo
