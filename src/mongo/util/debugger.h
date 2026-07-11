// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

// Sets SIGTRAP handler to launch debugger
// Noop unless on *NIX and compiled with MONGO_CONFIG_DEBUG_BUILD
void setupSIGTRAPforDebugger();

void waitForDebugger();

[[MONGO_MOD_PUBLIC]] void breakpoint();

bool isDebuggerActive();

}  // namespace mongo
