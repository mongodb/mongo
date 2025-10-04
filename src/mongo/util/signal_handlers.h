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

#include "mongo/util/duration.h"

namespace mongo {

enum class LogFileStatus {
    kNeedToRotateLogFile,
    kNoLogFileToRotate,
};

/**
 * Sets up handlers for signals and other events like terminate and new_handler.
 *
 * This must be called very early in main, before runGlobalInitializers().
 */
void setupSignalHandlers();

/**
 * Starts the thread to handle asynchronous signals.
 *
 * This must be the first thread started from the main thread.
 */
void startSignalProcessingThread(LogFileStatus rotate = LogFileStatus::kNeedToRotateLogFile);

/**
 * Starts a thread that randomly picks a victim thread at randomized intervals and sends a signal
 * to that thread in an effort to cause system calls to randomly fail with EINTR. Only works
 * on linux, does nothing on other platforms.
 *
 * The given period is the average interval at which victim threads are signalled.
 */
void startSignalTestingThread(Milliseconds period);

/*
 * Uninstall the Control-C handler
 *
 * Windows Only
 * Used by nt services to remove the Control-C handler after the system knows it is running
 * as a service, and not as a console program.
 */
void removeControlCHandler();

}  // namespace mongo
