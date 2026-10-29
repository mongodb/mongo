// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::logical_sessions {

static constexpr int64_t kAverageSessionDocSizeBytes = 200;
static constexpr int64_t kDesiredDocsInChunks = 1000;
static constexpr int64_t kMaxChunkSizeBytes = kAverageSessionDocSizeBytes * kDesiredDocsInChunks;

}  // namespace mongo::logical_sessions
