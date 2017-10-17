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

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"

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

    /**
     * Moves the OperationContext of `ctx` from its current OperationContextGroup into *this.
     * Do this to protect an OperationContext from being interrupted along with the rest of its
     * group, or to expose `ctx` to this->interrupt().  Taking from a Context already in *this is
     * equivalent to moving from `ctx`. Taking a moved-from Context yields another moved-from
     * Context.
     */
    Context take(Context ctx);

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

    stdx::mutex _lock;
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
