// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/unittest/unittest_main_core.h"

namespace mongo::dbtests {
unittest::MainProgress initializeDbTests(std::vector<std::string> argVec);
int runDbTests(unittest::MainProgress& progress);
}  // namespace mongo::dbtests
