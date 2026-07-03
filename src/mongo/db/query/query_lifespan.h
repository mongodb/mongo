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
#include "mongo/util/decorable.h"

#include <memory>
#include <utility>

namespace mongo {

/**
 * Represents the lifetime of a single logical query and owns node-local state associated with it.
 * A QueryLifespan is a Decorable accessed through an OperationContext, but its lifetime is
 * deliberately decoupled from any individual OperationContext. The originating command's cursor
 * retains a handle, and each subsequent getMore binds the lifespan to its OperationContext,
 * so decorated state persists from the initial command until the cursor is exhausted or destroyed
 * rather than being discarded at the end of each request.
 *
 * Selecting the appropriate scope for query state:
 *
 *   OperationContext:  A single request, such as one command invocation or getMore. State is
 *                      released when the request completes.
 *   ExpressionContext: A single pipeline or sub-query. One logical query may comprise many
 *                      ExpressionContexts (for example $lookup sub-pipelines, sub-executors, or
 *                      direct-client sub-queries), so it is unsuitable for state that must remain
 *                      consistent across all of them.
 *   QueryLifespan:     The query as a whole. A single instance is shared by every sub-executor and
 *                      sub-pipeline of the query and persists across getMore commands.
 *
 * The "shared by every sub-executor and sub-pipeline" guarantee holds only because those
 * sub-executors (for example $lookup, $graphLookup, $unionWith, and DBDirectClient sub-queries)
 * reuse the parent's exact OperationContext*; QueryLifespan itself does not enforce this. Code
 * that runs part of a query's execution on a distinct OperationContext must explicitly call
 * 'bind()' onto that new OperationContext, or the QueryLifespan's decorated state will not be
 * visible there.
 *
 * As a guideline, state that must remain consistent across all of a query's sub-pipelines and
 * sub-executors, or that must outlive a single OperationContext (for example resolved query
 * settings, knob configuration, or memory-usage tracking), belongs on the QueryLifespan rather
 * than on the ExpressionContext.
 *
 * Scope and usage constraints:
 *
 *   1. A QueryLifespan is node-local and is never propagated from a router to a shard; each node
 *      maintains its own instance. Reconciling state between a router and its shards is a separate
 *      concern.
 *   2. Consumers should not reference or pass a QueryLifespan directly. Instead, declare a
 *      decoration within a .cpp file and expose it through a static accessor. This keeps the
 *      lifespan an implementation detail and prevents it from becoming a general-purpose context
 *      object:
 *
 *          namespace {
 *          auto decoration = QueryLifespan::declareOpCtxDecoration<MyQueryState>();
 *          }  // namespace
 *
 *          MyQueryState& MyQueryState::get(OperationContext* opCtx) {
 *              return decoration(opCtx);
 *          }
 */
class QueryLifespan final : public Decorable<QueryLifespan>,
                            public std::enable_shared_from_this<QueryLifespan> {
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    using Handle = std::shared_ptr<QueryLifespan>;
    QueryLifespan(Passkey) {}

    /**
     * Returns the QueryLifespan for 'opCtx', creating it on first access. The reference is
     * unowned and should be consumed immediately rather than stored; use 'handle()' if it needs
     * to outlive the current scope.
     */
    static QueryLifespan& get(OperationContext* opCtx);

    /**
     * Binds this lifespan onto 'opCtx', making its decorations accessible through that
     * OperationContext. Called when a new request adopts an existing query, for example during
     * getMore setup ('setUpOperationContextAndCurOpStateForGetMore').
     */
    void bind(OperationContext* opCtx);

    /**
     * Returns a handle that keeps this lifespan alive after the originating OperationContext ends.
     * The owning ClientCursor holds it so the state survives until the cursor is exhausted or
     * destroyed. This means decorated state inherits the owning cursor's full lifetime, including
     * tailable or noTimeout cursors that can live indefinitely, so avoid attaching state whose
     * size is unbounded or that must not outlive a single request.
     */
    [[nodiscard]] Handle handle();

    /**
     * Scoped guard that runs the current scope under 'lifespan' -- installing it on 'opCtx' -- and
     * restores whatever lifespan 'opCtx' previously carried on destruction, leaving 'opCtx' as it
     * was found. Obtain one from 'ClientCursor::bindQueryLifespan'.
     *
     * A getMore uses this to run under its cursor's lifespan without clobbering a transient
     * lifespan the opCtx already carried -- e.g. one lazily created by auth's localhost-bypass
     * 'usersInfo' check run via DBDirectClient during startRequest. Restoring on destruction also
     * protects a parent operation when the getMore runs on a shared OperationContext (via
     * DBDirectClient).
     */
    class [[nodiscard]] AlternativeQueryRegion {
    public:
        AlternativeQueryRegion(OperationContext* opCtx, const Handle& lifespan);
        ~AlternativeQueryRegion();

        AlternativeQueryRegion(const AlternativeQueryRegion&) = delete;
        AlternativeQueryRegion& operator=(const AlternativeQueryRegion&) = delete;
        AlternativeQueryRegion(AlternativeQueryRegion&&) noexcept;
        AlternativeQueryRegion& operator=(AlternativeQueryRegion&&) noexcept;

    private:
        // Pointer (not reference) so a moved-from region can be disarmed by nulling it; the
        // destructor only restores when '_slot' is non-null.
        Handle* _slot;
        Handle _saved;
    };

    /**
     * A decoration that is always reached through an OperationContext. Hides the lifespan lookup so
     * consumers never name QueryLifespan: declare one with 'declareOpCtxDecoration<T>()' and access
     * it as 'decoration(opCtx)'.
     */
    template <typename T>
    class OpCtxDecoration {
    public:
        T& operator()(OperationContext* opCtx) const {
            return QueryLifespan::get(opCtx).decoration(_token);
        }

    private:
        Decoration<T> _token = QueryLifespan::declareDecoration<T>();
    };

    template <typename T>
    [[nodiscard]] static OpCtxDecoration<T> declareOpCtxDecoration() {
        static bool declared = false;
        invariant(!std::exchange(declared, true),
                  "declareOpCtxDecoration<T> called more than once for the same T");
        return {};
    }
};

}  // namespace mongo
