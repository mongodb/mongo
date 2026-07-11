// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>  // size_t

namespace mongo {

/**
 * Statistics used to measure work done while scanning a wildcard index multikey metadata reserved
 * index range.
 */
struct MultikeyMetadataAccessStats {
    size_t numSeeks{0};
    size_t keysExamined{0};
};

}  // namespace mongo
