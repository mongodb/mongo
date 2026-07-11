// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/sorter_file_name.h"

#include "mongo/platform/atomic.h"
#include "mongo/platform/random.h"

#include <fmt/format.h>

namespace mongo::sorter {
boost::filesystem::path nextFileName(boost::filesystem::path path) {
    static Atomic<unsigned> fileCounter;
    static const uint64_t randomSuffix = SecureRandom().nextUInt64();
    path /= fmt::format("extsort.{}-{}", fileCounter.fetchAndAdd(1), randomSuffix);
    return path;
}
}  // namespace mongo::sorter
