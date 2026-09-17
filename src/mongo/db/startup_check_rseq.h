// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>

namespace mongo {

void validateRseqKernelCompat();
[[MONGO_MOD_FILE_PRIVATE]] bool isKernelVersionSafeForTCMallocPerCPUCache(std::string_view release);
[[MONGO_MOD_FILE_PRIVATE]] boost::optional<std::string_view>
ubuntuKernelVersionFromVersionSignature(std::string_view versionSignature);

}  // namespace mongo
