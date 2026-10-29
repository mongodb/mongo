// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

/**
 * A struct with a static helper functions that can initialize the global FCV version to latest for
 * executing benchmarks. Initializes FCV version to latest. Initializing the FCV version is
 * necessary for some benchmarks when they acquire FCV snapshots.
 */
struct QueryFCVEnvironmentForTest {
    /**
     * Initialize the environment by setting the global FCV version to latest.
     */
    static void setUp();
};

}  // namespace mongo
