// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Marks a thread as idle while in scope. Prefer to use the macro below.
 *
 * Our debugger scripts can hide idle threads when dumping all stacks. You should mark threads as
 * idle when printing the stack would just be unhelpful noise. IdleThreadBlocks are not allowed to
 * nest. Each thread should generally have at most one possible place where it it is considered
 * idle.
 */
class IdleThreadBlock {
public:
    IdleThreadBlock(const char* location) {
        beginIdleThreadBlock(location);
    }
    ~IdleThreadBlock() {
        endIdleThreadBlock();
    }
    IdleThreadBlock(const IdleThreadBlock&) = delete;
    IdleThreadBlock& operator=(const IdleThreadBlock&) = delete;

    // These should not be called by mongo C++ code. They are only public to allow exposing this
    // functionality to a C api.
    static void beginIdleThreadBlock(const char* location);
    static void endIdleThreadBlock();
};

#define MONGO_IDLE_THREAD_BLOCK_STR1_(x) #x
#define MONGO_IDLE_THREAD_BLOCK_STR_(x) MONGO_IDLE_THREAD_BLOCK_STR1_(x)

/**
 * Marks a thread idle for the rest of the current scope and passes file:line as the location.
 */
#define MONGO_IDLE_THREAD_BLOCK \
    IdleThreadBlock markIdle(__FILE__ ":" MONGO_IDLE_THREAD_BLOCK_STR_(__LINE__))


}  // namespace mongo
