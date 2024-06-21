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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <deque>
#include <new>

#include <boost/optional/optional.hpp>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tracing_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

class BasicTracerFactory final : public Tracer::Factory {
public:
    BasicTracerFactory(std::string name, Tracer* tracer)
        : _name(std::move(name)), _tracer(tracer) {}

    class BasicSpan final : public Tracer::Span {
    public:
        BasicSpan(BSONObjBuilder bob, TickSource::Tick tracerStart, std::shared_ptr<Tracer> tracer)
            : _bob(std::move(bob)), _tracerStart(tracerStart), _tracer(std::move(tracer)) {
            _bob.append("startedMicros"_sd, durationCount<Microseconds>(_now()));
        }

        ~BasicSpan() override {
            _spans = boost::none;
            _bob.append("stoppedMicros"_sd, durationCount<Microseconds>(_now()));
        }

        BSONObjBuilder makeSubSpan(std::string name) {
            if (!_spans) {
                _spans.emplace(_bob.subobjStart("spans"_sd));
            }
            return _spans->subobjStart(name);
        }

        Microseconds _now() const {
            auto ts = _tracer->getTickSource();
            return ts->ticksTo<Microseconds>(ts->getTicks() - _tracerStart);
        }

    private:
        BSONObjBuilder _bob;
        boost::optional<BSONObjBuilder> _spans;
        TickSource::Tick _tracerStart;
        const std::shared_ptr<Tracer> _tracer;
    };

    Tracer::ScopedSpan startSpan(std::string name) override {
        if (_spans.empty()) {
            // We're starting a new root span, so erase the most recent trace.
            _trace = boost::none;
            _tracerStart = _tracer->getTickSource()->getTicks();
        }

        auto span = std::make_unique<BasicSpan>(
            _makeObjBuilder(std::move(name)), _tracerStart, _tracer->shared_from_this());
        _spans.push_back(span.get());

        return Tracer::ScopedSpan(span.release(), [this](Tracer::Span* span) {
            invariant(span == _spans.back(), "Spans must go out of scope in the order of creation");
            _spans.pop_back();
            delete span;

            if (_spans.empty()) {
                _trace.emplace(_builder->obj());
                _builder = boost::none;
            }
        });
    }

    boost::optional<BSONObj> getLatestTrace() const override {
        return _trace;
    }

private:
    BSONObjBuilder _makeObjBuilder(std::string spanName) {
        if (_spans.empty()) {
            // This is the root span.
            _builder.emplace();
            _builder->append("tracer"_sd, _name);
            return _builder->subobjStart(spanName);
        } else {
            // This is a child for the currently active span.
            auto& activeSpan = *_spans.back();
            return activeSpan.makeSubSpan(std::move(spanName));
        }
    }


    const std::string _name;
    Tracer* const _tracer;
    TickSource::Tick _tracerStart;

    std::deque<BasicSpan*> _spans;
    boost::optional<BSONObjBuilder> _builder;
    boost::optional<BSONObj> _trace;
};

/**
 * Trace Event Format
 * Defined: https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/edit
 *
 * Consumed by Google Chromium Tracing, Catapult, and https://perfetto.dev
 */
class TraceEventTracerFactory final : public Tracer::Factory {
public:
    TraceEventTracerFactory(std::string name, Tracer* tracer)
        : _name(std::move(name)), _tracer(tracer) {}

    class BasicSpan final : public Tracer::Span {
    public:
        BasicSpan(TraceEventTracerFactory* factory, StringData name, std::shared_ptr<Tracer> tracer)
            : _factory(factory), _tracer(std::move(tracer)) {

            _factory->_arrayBuilder->append(BSON("name" << name << "ph"
                                                        << "B"
                                                        << "ts" << _nowFractionalMillis() << "pid"
                                                        << 1 << "tid" << 1));
        }

        ~BasicSpan() override {
            _spans = boost::none;
            _factory->_arrayBuilder->append(BSON("ph"
                                                 << "E"
                                                 << "ts" << _nowFractionalMillis() << "pid" << 1
                                                 << "tid" << 1));
        }

        double _nowFractionalMillis() const {
            auto ts = _tracer->getTickSource();
            return static_cast<double>(durationCount<Microseconds>(
                       ts->ticksTo<Microseconds>(ts->getTicks() - _factory->_startBase))) /
                1000;
        }

    private:
        TraceEventTracerFactory* _factory;
        boost::optional<BSONObjBuilder> _spans;
        const std::shared_ptr<Tracer> _tracer;
    };

    Tracer::ScopedSpan startSpan(std::string name) override {
        if (_spans.empty()) {
            // We're starting a new root span, so erase the most recent trace.
            _trace = boost::none;
            _builder.emplace(BSONObjBuilder());
            _arrayBuilder.emplace(_builder->subarrayStart("traceEvents"));
            _startBase = _tracer->getTickSource()->getTicks();
        }

        auto span = std::make_unique<BasicSpan>(this, name, _tracer->shared_from_this());

        _spans.push_back(span.get());

        return Tracer::ScopedSpan(span.release(), [this](Tracer::Span* span) {
            invariant(span == _spans.back(), "Spans must go out of scope in the order of creation");
            _spans.pop_back();
            delete span;

            if (_spans.empty()) {
                // Finalize by appending the common metadata
                _arrayBuilder->done();

                _builder->append("displayTimeUnit", "ms");

                _trace.emplace(_builder->obj());
                _builder = boost::none;
            }
        });
    }

    boost::optional<BSONObj> getLatestTrace() const override {
        return _trace;
    }

private:
    const std::string _name;
    Tracer* const _tracer;

    TickSource::Tick _startBase;
    std::deque<BasicSpan*> _spans;
    boost::optional<BSONObjBuilder> _builder;
    boost::optional<BSONArrayBuilder> _arrayBuilder;
    boost::optional<BSONObj> _trace;
};

boost::optional<TracerProvider>& getTraceProvider() {
    static StaticImmortal<boost::optional<TracerProvider>> provider;
    return *provider;
}

MONGO_INITIALIZER(InitializeTraceProvider)(InitializerContext*) {
    // The following checks if the tracer is already initialized. This allows declaring another
    // `MONGO_INITIALIZER` that can precede the following and initialize the tracer with a custom
    // clock source. This is especially helpful for mocking the clock source in unit-tests.
    if (auto& provider = getTraceProvider(); provider.has_value()) {
        return;
    }

    LOGV2_OPTIONS(5970001,
                  {logv2::LogTag::kStartupWarnings},
                  "Operation tracing is enabled. This may have performance implications.");
    TracerProvider::initialize(makeSystemTickSource());  // NOLINT
}

template <typename FactoryType>
auto makeTracer(std::string name, TickSource* tickSource) {
    return std::make_shared<Tracer>(name, tickSource, [](std::string name, Tracer* tracer) {
        return std::make_unique<FactoryType>(std::move(name), tracer);
    });
}

}  // namespace

Tracer::Tracer(std::string name,
               TickSource* tickSource,
               std::function<std::unique_ptr<Factory>(std::string, Tracer*)> maker)
    : _tickSource(tickSource) {
    _factory = maker(std::move(name), this);
}

void TracerProvider::initialize(std::unique_ptr<TickSource> tickSource) {
    auto& provider = getTraceProvider();
    invariant(!provider.has_value(), "already initialized");
    provider.emplace(TracerProvider(std::move(tickSource)));
}

TracerProvider& TracerProvider::get() {
    auto& provider = getTraceProvider();
    invariant(provider.has_value(), "not initialized");
    return provider.value();
}

std::shared_ptr<Tracer> TracerProvider::getTracer(std::string name) {
    return makeTracer<BasicTracerFactory>(std::move(name), _tickSource.get());
}

std::shared_ptr<Tracer> TracerProvider::getEventTracer(std::string name) {
    return makeTracer<TraceEventTracerFactory>(std::move(name), _tickSource.get());
}

}  // namespace mongo
