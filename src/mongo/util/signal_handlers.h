// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

enum class LogFileStatus {
    kNeedToRotateLogFile,
    kNoLogFileToRotate,
};

struct LogRotationState {
    static constexpr auto kNever = static_cast<time_t>(-1);
    LogFileStatus logFileStatus;
    time_t previous;
};

/**
 * Sets up handlers for signals and other events like terminate and new_handler.
 *
 * This must be called very early in main, before runGlobalInitializers().
 */
void setupSignalHandlers();

/**
 * Registers a user-defined callback to be invoked on the asynchronous signal handling thread
 * after processing of a signal, but before process exit.
 *
 * Clobbers the previously-registered callback. Only called by asynchronous signals expected
 * to terminate the process.
 */
[[MONGO_MOD_PUBLIC]] void setSignalPostProcessingCallback_forTest(std::function<void()> cb);

/**
 * Registers a user-defined callback to be invoked on the asynchronous signal handling thread
 * when the log rotation signal is received.
 *
 * Clobbers the previously-registered callback. Should only be called during server
 * intialization. Expected to be called once per process.
 */
void setLogRotationCallback(std::function<void(LogRotationState*)> cb);

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
