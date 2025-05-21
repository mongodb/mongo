/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <memory>

namespace mongo {
namespace tracing {

/**
 * Traceable holds the SpanContext necessary to create parent-child relationships between spans
 * in a trace. It holds the SpanContext from the current active span. When a new span is created,
 * it inherits from this SpanContext (to ensure it is a child of the active span) and replaces the
 * SpanContext with its own (to ensure future spans are its children).
 *
 * Traceable is NOT thread-safe, it MUST NOT be used by multiple threads without additional
 * synchronisation.
 *
 * Example:
 *     Traceable t;
 *     Span A{&t}; // t->_context == A's context.
 *     Traceable t2{t}; t2->_context == A's context;
 *     {
 *         Span B{&t}; // t->_context == B's context.
 *         Span C{&t}; // t->_context == C's context.
 *         Span E{&t2}; // t2->_context == E's context.
 *     } // Destructors. t->_context == A's context. t2->_context == A's context.
 *     Span D{&t}; // t->_context == C's context.
 *
 * In this example, A is the parent span of B, D and *E* while B is the parent of C. Traceable t2
 * gives an example of parallel spans : E represents an operation that is executing simultaneously
 * to B and C. Therefore, E uses a different Traceable (copied from t) to ensure it is *not* a
 * child of B or C.
 */
class Traceable {
public:
    /**
     * Base class to represent a SpanContext and ensure users of Traceable do not need to be linked
     * with third-party libraries.
     */
    class SpanContext {
    public:
        virtual ~SpanContext() = default;
    };

    void setActiveContext(std::shared_ptr<SpanContext> ctx) {
        _context = std::move(ctx);
    }

    /**
     * Return the active SpanContext (i.e. the context from the most recent alive span associated
     * with this Traceable).
     */
    const std::shared_ptr<SpanContext>& getActiveContext() const {
        return _context;
    }

private:
    std::shared_ptr<SpanContext> _context;
};

}  // namespace tracing
}  // namespace mongo
