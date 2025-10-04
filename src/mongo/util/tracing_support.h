/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/util/tick_source.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Handles span creation and provides a compatible interface to `opentelemetry::trace::Tracer`.
 * This class does not support concurrent accesses, and should not be used in production.
 *
 * Spans are organized in a hierarchy. Once a new span is created, through calling `startSpan()`,
 * it will be added as a child to the active span, and replaces its parent as the new active span.
 * When there is no active span, the newly created span is considered as the root span.
 *
 * Once the root span goes out of scope, the collected trace is serialized into a BSON object, and
 * may be retrieved through `getLatestTrace()`. The trace object remains valid until a new root span
 * is created. This interface is not compatible with `opentelemetry::trace::Tracer`, and fills the
 * gap for `opentelemetry` exporters.
 *
 * Here is an example on how to create spans and retrieve traces:
 * ```
 * void f1(std::shared_ptr<Tracer> tracer) {
 *     auto root = tracer->startSpan("root");
 *     sleepFor(Milliseconds(1));
 *     {
 *         auto child = tracer->startSpan("child");
 *         sleepFor(Milliseconds(2));
 *     }
 * }
 *
 * void f2() {
 *     auto tracer = TracerProvider::get().getTracer("myTracer");
 *     f1(tracer);
 *     BSONObj trace = tracer->getLatestTrace();
 * }
 * ```
 *
 * The above code will produce the following `BSONObj`:
 * ```
 * {
 *     "tracer": "myTracer",
 *     "root": {
 *         "startedMicros": 0,
 *         "spans": {
 *             "child": {
 *                 "startedMicros": 1,
 *                 "stoppedMicros": 3,
 *             },
 *         },
 *         "stoppedMicros": 3,
 *     },
 * }
 * ```
 */
class Tracer : public std::enable_shared_from_this<Tracer> {
public:
    class Span {
    public:
        virtual ~Span() = default;
    };

    using ScopedSpan = std::unique_ptr<Span, std::function<void(Span*)>>;

    class Factory {
    public:
        virtual ~Factory() = default;

        virtual ScopedSpan startSpan(std::string) = 0;

        virtual boost::optional<BSONObj> getLatestTrace() const = 0;
    };

    Tracer(std::string name,
           TickSource* tickSource,
           std::function<std::unique_ptr<Factory>(std::string, Tracer*)> maker);

    ScopedSpan startSpan(std::string name) {
        return _factory->startSpan(std::move(name));
    }

    boost::optional<BSONObj> getLatestTrace() const {
        return _factory->getLatestTrace();
    }

    TickSource* getTickSource() {
        return _tickSource;
    }

private:
    TickSource* const _tickSource;
    std::unique_ptr<Factory> _factory;
};

/**
 * The factory class for constructing instances of `Tracer`.
 * Consider the following before using this class:
 * Must be initialized first by calling `initialize()`, and implements the singleton pattern.
 * Once initialized, multiple threads my concurrently call into `get()` and `getTracer()`.
 */
class TracerProvider {
public:
    explicit TracerProvider(std::unique_ptr<TickSource> tickSource)
        : _tickSource(std::move(tickSource)) {}

    static void initialize(std::unique_ptr<TickSource> tickSource);

    static TracerProvider& get();

    std::shared_ptr<Tracer> getTracer(std::string name);

    /**
     * Get a tracer that outputs Chrome's Trace Event Format
     */
    std::shared_ptr<Tracer> getEventTracer(std::string name);

private:
    std::unique_ptr<TickSource> _tickSource;
};

}  // namespace mongo
