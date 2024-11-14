/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <functional>

#include "mongo/util/processinfo.h"
#include "mongo/util/tracking/allocator.h"

namespace mongo::tracking {

/**
 * A Context is a factory style class that constructs Allocator objects under a
 * single instance of AllocatorStats and provides access to these stats.
 */
class Context {
public:
    Context() = default;
    ~Context() = default;

    template <class T>
    Allocator<T> makeAllocator() {
        return Allocator<T>(_stats);
    }

    AllocatorStats& stats() {
        return _stats;
    }

    uint64_t allocated() const {
        return _stats.allocated();
    }

private:
    AllocatorStats _stats{ProcessInfo::getNumLogicalCores() * 2};
};

}  // namespace mongo::tracking
