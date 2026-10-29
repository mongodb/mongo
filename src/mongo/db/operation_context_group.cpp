// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_context_group.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

namespace mongo {

namespace {

using ContextTable = std::vector<ServiceContext::UniqueOperationContext>;

auto find(ContextTable& contexts, OperationContext* cp) {
    auto it = std::find_if(
        contexts.begin(), contexts.end(), [cp](auto const& opCtx) { return opCtx.get() == cp; });
    invariant(it != contexts.end());
    return it;
}

void interruptOne(OperationContext* opCtx, ErrorCodes::Error code) {
    ClientLock lk(opCtx->getClient());
    opCtx->getServiceContext()->killOperation(lk, opCtx, code);
}

}  // namespace

// OperationContextGroup::Context

OperationContextGroup::Context::Context(OperationContext& ctx, OperationContextGroup& group)
    : _opCtx(ctx), _ctxGroup(group) {}

void OperationContextGroup::Context::discard() {
    if (!_movedFrom) {
        std::lock_guard<std::mutex> lk(_ctxGroup._lock);
        auto it = find(_ctxGroup._contexts, &_opCtx);
        _ctxGroup._contexts.erase(it);
        _movedFrom = true;
    }
}

// OperationContextGroup

auto OperationContextGroup::makeOperationContext(Client& client) -> Context {
    return adopt(client.makeOperationContext());
}

auto OperationContextGroup::adopt(UniqueOperationContext opCtx) -> Context {
    auto cp = opCtx.get();
    invariant(cp);
    std::lock_guard<std::mutex> lk(_lock);
    _contexts.emplace_back(std::move(opCtx));
    return Context(*cp, *this);
}

void OperationContextGroup::interrupt(ErrorCodes::Error code) {
    invariant(code);
    std::lock_guard<std::mutex> lk(_lock);
    for (auto&& uniqueOperationContext : _contexts) {
        interruptOne(uniqueOperationContext.get(), code);
    }
}

bool OperationContextGroup::isEmpty() {
    std::lock_guard<std::mutex> lk(_lock);
    return _contexts.empty();
}

}  // namespace mongo
