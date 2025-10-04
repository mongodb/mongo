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

namespace mongo {

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
