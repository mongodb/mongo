/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"

namespace mongo {

/**
 * An interface for an executor class capable of evaluating a function over a given window
 * definition. The function must expose an accumulate-type interface and potentially a remove
 * interface depending on the window bounds.
 *
 * This class is also responsible for handling partition edge cases; for instance when either the
 * lower bound falls before the start of the partition or the upper bound spills off of the end.
 */
class WindowFunctionExec {
public:
    WindowFunctionExec(PartitionIterator* iter) : _iter(iter){};

    /**
     * Retrieve the next value computed by the window function.
     */
    virtual Value getNext() = 0;

    /**
     * Resets the executor as well as any execution state to a clean slate.
     */
    virtual void reset() = 0;

protected:
    PartitionIterator* _iter;
};

}  // namespace mongo
