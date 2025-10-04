/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once
#include "mongo/base/status.h"

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
bool getTestCommandsEnabled();

void setTestCommandsEnabled(bool b);

/** Callback for the enableTestCommands server parameter. */
Status onUpdateTestCommandsEnabled(bool b);

}  // namespace mongo
