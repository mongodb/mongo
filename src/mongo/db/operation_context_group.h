// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * OperationContextGroup maintains a collection of operation contexts so that they may all be killed
 * on a common event (typically stepdown).  Its public member functions serialize access to private
 * data members.
 */
class OperationContextGroup {
    OperationContextGroup(OperationContextGroup const&) = delete;
    OperationContextGroup(OperationContextGroup&&) = delete;
    void operator=(OperationContextGroup const&) = delete;
    void operator=(OperationContextGroup&&) = delete;

public:
    using UniqueOperationContext = ServiceContext::UniqueOperationContext;
    class Context;

    OperationContextGroup() = default;

    ~OperationContextGroup() {
        invariant(isEmpty());
    }

    /**
     * Makes an OperationContext on `client` and returns a Context object to track it.  On
     * destruction of the returned Context, the OperationContext is destroyed and its corresponding
     * entry in *this is erased.  If *this has been interrupted already, the new context will be
     * interrupted immediately.
     */
    Context makeOperationContext(Client& client);

    /**
     * Takes ownership of the OperationContext from `ctx`, and returns a Context object to track it.
     * On destruction of the Context, its entry in *this is erased and its corresponding
     * OperationContext is destroyed. If *this has been interrupted already, `ctx` will be
     * interrupted immediately.
     */
    Context adopt(UniqueOperationContext ctx);

    /*
     * Interrupts all the OperationContexts maintained in *this.
     */
    void interrupt(ErrorCodes::Error);

    /**
     * Reports whether the group has any OperationContexts.  This must be true before the destructor
     * is called.  Its usefulness is typically limited to invariants.
     */
    bool isEmpty();

private:
    friend class Context;

    std::mutex _lock;
    std::vector<UniqueOperationContext> _contexts;

};  // class OperationContextGroup

/**
 * Context tracks one OperationContext*, and on destruction unregisters and destroys the associated
 * OperationContext.  May be used as if it were an OperationContext*.
 *
 * The lifetime of an OperationContextGroup::Context object must not exceed that of its
 * OperationContextGroup, unless it has been moved from, taken from (see
 * OperationContextGroup::take), or discarded.
 */
class OperationContextGroup::Context {
    Context() = delete;
    Context(Context const&) = delete;
    void operator=(Context const&) = delete;
    void operator=(Context&&) = delete;

public:
    Context(Context&& ctx) : _opCtx(ctx._opCtx), _ctxGroup(ctx._ctxGroup) {
        ctx._movedFrom = true;
    }
    ~Context() {
        discard();
    }

    /**
     * Returns a pointer to the tracked OperationContext, or nullptr if *this has been moved from.
     */
    OperationContext* opCtx() const {
        return _movedFrom ? nullptr : &_opCtx;
    }

    /**
     * These enable treating a Context as if it were an OperationContext*.
     */
    operator OperationContext*() const {
        dassert(!_movedFrom);
        return &_opCtx;
    }
    OperationContext* operator->() const {  // because op-> will not use the conversion
        dassert(!_movedFrom);
        return &_opCtx;
    }

    /**
     * Destroys and unregisters the corresponding OperationContext.  After this operation, *this is
     * an xvalue, and can only be destroyed.
     */
    void discard();

private:
    friend class OperationContextGroup;

    Context(OperationContext& ctx, OperationContextGroup& group);

    bool _movedFrom = false;
    OperationContext& _opCtx;
    OperationContextGroup& _ctxGroup;

};  // class OperationContextGroup::Context

}  // namespace mongo
