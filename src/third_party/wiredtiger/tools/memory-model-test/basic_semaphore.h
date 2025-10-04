/*
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 *
 * This file contains a basic implementation of the C++ 20 std::binary_semaphore which is
 * useful for compilers which don't support this feature.
 */

#pragma once

#include <mutex>
#include <condition_variable>

class basic_semaphore {
public:
    explicit basic_semaphore(int count = 0) : _count(count) {};

    void acquire() {
        std::unique_lock<std::mutex> lock(_mutex);
        while(_count == 0)
            _condition_variable.wait(lock);
        _count--;
    }

    void release() {
        std::lock_guard<std::mutex> lock(_mutex);
        _count++;
        _condition_variable.notify_one();
    }

private:
    std::mutex _mutex;
    std::condition_variable _condition_variable;
    int _count;
};
