/**
 *    Copyright (C) 2017 Mongodb Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/operation_context_group.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

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
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    opCtx->getServiceContext()->killOperation(opCtx, code);
}

}  // namespace

// OperationContextGroup::Context

OperationContextGroup::Context::Context(OperationContext& ctx, OperationContextGroup& group)
    : _opCtx(ctx), _ctxGroup(group) {}

void OperationContextGroup::Context::discard() {
    if (!_movedFrom) {
        stdx::lock_guard<stdx::mutex> lk(_ctxGroup._lock);
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
    stdx::lock_guard<stdx::mutex> lk(_lock);
    _contexts.emplace_back(std::move(opCtx));
    return Context(*cp, *this);
}

auto OperationContextGroup::take(Context ctx) -> Context {
    if (ctx._movedFrom || &ctx._ctxGroup == this) {
        return ctx;
    }
    {
        stdx::lock_guard<stdx::mutex> lk(_lock);
        auto it = find(ctx._ctxGroup._contexts, &ctx._opCtx);
        _contexts.emplace_back(std::move(*it));
        ctx._ctxGroup._contexts.erase(it);
    }
    ctx._movedFrom = true;
    return Context(ctx._opCtx, *this);
}

void OperationContextGroup::interrupt(ErrorCodes::Error code) {
    invariant(code);
    stdx::lock_guard<stdx::mutex> lk(_lock);
    for (auto&& uniqueOperationContext : _contexts) {
        interruptOne(uniqueOperationContext.get(), code);
    }
}

bool OperationContextGroup::isEmpty() {
    stdx::lock_guard<stdx::mutex> lk(_lock);
    return _contexts.empty();
}

}  // namespace mongo
