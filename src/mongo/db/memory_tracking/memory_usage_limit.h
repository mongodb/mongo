/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

/**
 * Holder for a tracker's maximum allowed memory usage. Currently only a byte count fixed at
 * construction time; the explicit constructor makes every site that snapshots a byte count into a
 * limit visible.
 *
 * 'get()' takes an OperationContext so that a follow-up change can teach a limit to resolve
 * against the operation's QueryKnobConfiguration without re-touching callers. For now the value is
 * fixed at construction and 'opCtx' is ignored.
 */
class [[MONGO_MOD_PUBLIC]] MemoryUsageLimit {
public:
    using MemoryKnob = QueryKnob<long long>;
    explicit MemoryUsageLimit(MemoryKnob knob);

    // TODO SERVER-131136: also accept a MemorySize so call sites can write
    // MemoryUsageLimit{MemorySize{"100MB"}} instead of a raw byte count.
    explicit MemoryUsageLimit(int64_t value) : _value(value) {}

    int64_t get(OperationContext*) const {
        return _value;
    }

private:
    int64_t _value;
};

}  // namespace mongo
