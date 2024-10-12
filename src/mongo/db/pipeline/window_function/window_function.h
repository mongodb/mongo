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

namespace mongo {

/**
 * A WindowFunctionState is a mutable, removable accumulator.
 *
 * Implementations must ensure that 'remove()' undoes 'add()' when called in FIFO order.
 * For example:
 *     'add(x); add(y); remove(x)' == 'add(y)'
 *     'add(a); add(b); add(z); remove(a); remove(b)' == 'add(z)'
 */
class WindowFunctionState {
public:
    WindowFunctionState(ExpressionContext* const expCtx) : _expCtx(expCtx) {}
    virtual ~WindowFunctionState() = default;
    virtual void add(Value) = 0;
    virtual void remove(Value) = 0;
    virtual Value getValue() const = 0;
    virtual void reset() = 0;
    size_t getApproximateSize() {
        tassert(5414200, "_memUsageBytes not set for function", _memUsageBytes != 0);
        return _memUsageBytes;
    }

protected:
    ExpressionContext* _expCtx;
    size_t _memUsageBytes = 0;
};
}  // namespace mongo
