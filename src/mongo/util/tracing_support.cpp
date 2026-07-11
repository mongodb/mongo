// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/tracing_support.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/system_tick_source.h"

#include <deque>
#include <new>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

class BasicTracerFactory final : public Tracer::Factory {
public:
    BasicTracerFactory(std::string name, Tracer* tracer)
        : _name(std::move(name)), _tracer(tracer) {}

    class BasicSpan final : public Tracer::Span {
    public:
        BasicSpan(BSONObjBuilder bob, TickSource::Tick tracerStart, std::shared_ptr<Tracer> tracer)
            : _bob(std::move(bob)), _tracerStart(tracerStart), _tracer(std::move(tracer)) {
            _bob.append("startedMicros"sv, durationCount<Microseconds>(_now()));
        }

        ~BasicSpan() override {
            _spans = boost::none;
            _bob.append("stoppedMicros"sv, durationCount<Microseconds>(_now()));
        }

        BSONObjBuilder makeSubSpan(std::string name) {
            if (!_spans) {
                _spans.emplace(_bob.subobjStart("spans"sv));
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
            _builder->append("tracer"sv, _name);
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
 * Consumed by Google Chome Tracing, Catapult, and https://perfetto.dev
 */
class TraceEventTracerFactory final : public Tracer::Factory {
public:
    TraceEventTracerFactory(std::string name, Tracer* tracer)
        : _name(std::move(name)), _tracer(tracer) {}

    class BasicSpan final : public Tracer::Span {
    public:
        BasicSpan(TraceEventTracerFactory* factory,
                  std::string_view name,
                  std::shared_ptr<Tracer> tracer)
            : _factory(factory), _tracer(std::move(tracer)) {

            _factory->_arrayBuilder->append(BSON("name" << name << "ph"
                                                        << "B"
                                                        << "ts" << _nowFractionalMillis() << "pid"
                                                        << 1 << "tid" << 1));
        }

        ~BasicSpan() override {
            _spans = boost::none;
            _factory->_arrayBuilder->append(BSON("ph" << "E"
                                                      << "ts" << _nowFractionalMillis() << "pid"
                                                      << 1 << "tid" << 1));
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
