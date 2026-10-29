// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * If true, then testing commands are available. Defaults to false.
 *
 * Testing commands should conditionally register themselves by using
 * MONGO_REGISTER_COMMAND and .testOnly() (in src/mongo/db/commands.h),
 * which consults this flag:
 *
 *     class MyTestCommand ...;
 *     MONGO_REGISTER_COMMAND(MyTestCommand).forShard().testOnly();
 *
 * To make testing commands available by default, change the value to true before running any
 * mongo initializers:
 *
 *     int myMain(int argc, char** argv, char** envp) {
 *         setTestCommandsEnabled(true);
 *         ...
 *         runGlobalInitializersOrDie(argc, argv, envp);
 *         ...
 *     }
 */
[[MONGO_MOD_PUBLIC]] bool getTestCommandsEnabled();

[[MONGO_MOD_PUBLIC]] void setTestCommandsEnabled(bool b);

/** Callback for the enableTestCommands server parameter. */
Status onUpdateTestCommandsEnabled(bool b);

}  // namespace mongo
