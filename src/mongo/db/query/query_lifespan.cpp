// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_lifespan.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
auto opCtxDecoration = OperationContext::declareDecoration<QueryLifespan::Handle>();

// Resolves the opCtx's lifespan slot, tasserting 'opCtx' is non-null first so the slot pointer is
// formed without dereferencing a null opCtx.
QueryLifespan::Handle& slotFor(OperationContext* opCtx) {
    tassert(13020604, "AlternativeQueryRegion requires a non-null OperationContext", opCtx);
    return opCtxDecoration(opCtx);
}
}  // namespace

QueryLifespan& QueryLifespan::get(OperationContext* opCtx) {
    tassert(13020600, "QueryLifespan::get requires a non-null OperationContext", opCtx);
    auto& handle = opCtxDecoration(opCtx);
    if (!handle) {
        handle = std::make_shared<QueryLifespan>(Passkey());
    }
    return *handle;
}

void QueryLifespan::bind(OperationContext* opCtx) {
    tassert(13020601, "QueryLifespan::bind requires a non-null OperationContext", opCtx);
    auto& slot = opCtxDecoration(opCtx);
    tassert(13020602,
            "attempted to bind a QueryLifespan over a different, already-bound lifespan",
            !slot || slot.get() == this);
    slot = handle();
}

QueryLifespan::Handle QueryLifespan::handle() {
    return shared_from_this();
}

QueryLifespan::AlternativeQueryRegion::AlternativeQueryRegion(OperationContext* opCtx,
                                                              const Handle& lifespan)
    : _slot(&slotFor(opCtx)), _saved(lifespan) {
    // Install 'lifespan' and capture the opCtx's previous lifespan into '_saved'. swap is noexcept
    // and the tasserts run before it, so the ctor never leaves a partial mutation behind; the dtor
    // then restores via '_slot' alone, without touching the OperationContext again.
    std::swap(_saved, *_slot);
}

QueryLifespan::AlternativeQueryRegion::AlternativeQueryRegion(
    AlternativeQueryRegion&& other) noexcept
    : _slot(std::exchange(other._slot, nullptr)), _saved(std::move(other._saved)) {}

QueryLifespan::AlternativeQueryRegion& QueryLifespan::AlternativeQueryRegion::operator=(
    AlternativeQueryRegion&& other) noexcept {
    // Move-and-swap: 'tmp' adopts 'other' (disarming it), then takes over this region's current
    // obligation, so tmp's destructor eagerly restores our old slot exactly once as it goes out of
    // scope. Self-assignment is a harmless no-op.
    AlternativeQueryRegion tmp(std::move(other));
    std::swap(_slot, tmp._slot);
    std::swap(_saved, tmp._saved);
    return *this;
}

QueryLifespan::AlternativeQueryRegion::~AlternativeQueryRegion() {
    // A moved-from region has a null '_slot' and does nothing. Null after restoring so a repeated
    // destruction is a no-op.
    if (_slot) {
        std::swap(_saved, *_slot);
        _slot = nullptr;
    }
}

}  // namespace mongo
