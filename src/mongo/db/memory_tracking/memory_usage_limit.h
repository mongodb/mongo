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
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <variant>

namespace mongo {

/**
 * Holder for a tracker's maximum allowed memory usage. Either a byte count fixed at construction
 * time, or a query knob handle that the first 'get()' resolves against the operation's
 * QueryKnobConfiguration and latches; subsequent calls return the stored integer. Deferring
 * resolution to first use (instead of resolving eagerly at construction) lets the value reflect
 * any per-operation query-settings override applied to 'opCtx'.
 */
class [[MONGO_MOD_PUBLIC]] MemoryUsageLimit {
public:
    using MemoryKnob = QueryKnob<long long>;

    // TODO SERVER-131136: also accept a MemorySize so call sites can write
    // MemoryUsageLimit{MemorySize{"100MB"}} instead of a raw byte count.
    explicit MemoryUsageLimit(int64_t value) : _slot(value) {}

    /**
     * Stores 'knob' itself; the first 'get()' resolves it against the calling operation's
     * QueryKnobConfiguration. Registered query knobs are always 'long long'-typed (matching the
     * AtomicWord<long long> server parameters they mirror), a distinct type from 'int64_t' where
     * 'long' and 'long long' differ.
     */
    explicit MemoryUsageLimit(MemoryKnob knob) : _slot(knob) {}

    /**
     * Returns the limit. For a knob-backed limit, the knob is resolved against the operation's
     * QueryKnobConfiguration once, then latched (the slot is replaced with the resolved value) so
     * subsequent calls are a plain read. With a null 'opCtx' (e.g. expression evaluation on a
     * shared or detached ExpressionContext) the knob's global value is returned without latching.
     * For a fixed-value limit, 'opCtx' is ignored.
     *
     * Testing the 'int64_t' alternative (rather than the knob) keeps the latched fast path to a
     * single discriminant check with no 'bad_variant_access' throw stub: 'get_if' succeeding is
     * itself the proof that the dereference is valid.
     */
    MONGO_COMPILER_ALWAYS_INLINE int64_t get(OperationContext* opCtx) const {
        if (auto* value = std::get_if<int64_t>(&_slot); MONGO_likely(value != nullptr)) {
            return *value;
        }
        return _resolveAndLatch(opCtx);
    }

private:
    /**
     * Cold path: resolves the knob against the operation's QueryKnobConfiguration and latches the
     * result. Defined out-of-line to keep QueryKnobConfiguration out of this header.
     */
    int64_t _resolveAndLatch(OperationContext* opCtx) const;

    // Mutable so 'get()' can latch a resolved knob value in place on first read.
    mutable std::variant<int64_t, QueryKnob<long long>> _slot;
};

}  // namespace mongo
