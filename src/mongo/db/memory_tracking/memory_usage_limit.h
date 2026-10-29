// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
