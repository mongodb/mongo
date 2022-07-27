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


#include <deque>

#include "mongo/util/tracing_support.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/system_clock_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

class BasicTracerFactory final : public Tracer::Factory {
public:
    BasicTracerFactory(std::string name, Tracer* tracer)
        : _name(std::move(name)), _tracer(tracer) {}

    class BasicSpan final : public Tracer::Span {
    public:
        BasicSpan(BSONObjBuilder bob, std::shared_ptr<Tracer> tracer)
            : _bob(std::move(bob)), _tracer(std::move(tracer)) {
            _bob.append("started"_sd, _tracer->getClockSource()->now());
        }

        ~BasicSpan() {
            _spans = boost::none;
            _bob.append("stopped"_sd, _tracer->getClockSource()->now());
        }

        BSONObjBuilder makeSubSpan(std::string name) {
            if (!_spans) {
                _spans.emplace(_bob.subobjStart("spans"_sd));
            }
            return _spans->subobjStart(name);
        }

    private:
        BSONObjBuilder _bob;
        boost::optional<BSONObjBuilder> _spans;
        const std::shared_ptr<Tracer> _tracer;
    };

    Tracer::ScopedSpan startSpan(std::string name) override {
        if (_spans.empty()) {
            // We're starting a new root span, so erase the most recent trace.
            _trace = boost::none;
        }
        auto span = std::make_unique<BasicSpan>(_makeObjBuilder(std::move(name)),
                                                _tracer->shared_from_this());
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

    std::deque<BasicSpan*> _spans;
    boost::optional<BSONObjBuilder> _builder;
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
    TracerProvider::initialize(std::make_unique<SystemClockSource>());  // NOLINT
}

}  // namespace

Tracer::Tracer(std::string name, ClockSource* clkSource) : _clkSource(clkSource) {
    _factory = std::make_unique<BasicTracerFactory>(std::move(name), this);
}

void TracerProvider::initialize(std::unique_ptr<ClockSource> clkSource) {  // NOLINT
    auto& provider = getTraceProvider();
    invariant(!provider.has_value(), "already initialized");
    provider.emplace(TracerProvider(std::move(clkSource)));
}

TracerProvider& TracerProvider::get() {  // NOLINT
    auto& provider = getTraceProvider();
    invariant(provider.has_value(), "not initialized");
    return provider.value();
}

std::shared_ptr<Tracer> TracerProvider::getTracer(std::string name) {
    return std::make_shared<Tracer>(name, _clkSource.get());
}

}  // namespace mongo
