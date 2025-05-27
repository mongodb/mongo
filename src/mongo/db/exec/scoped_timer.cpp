/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/scoped_timer.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <type_traits>

namespace mongo {

class ScopedTimer::State {
public:
    virtual ~State() = default;
};

class ScopedTimer::CsState final : public State {
public:
    CsState(Nanoseconds* counter, ClockSource* cs) : counter{counter}, cs{cs}, start{cs->now()} {}
    ~CsState() override {
        *counter += cs->now() - start;
    }

private:
    Nanoseconds* counter;
    ClockSource* cs;
    Date_t start;
};

class ScopedTimer::TsState final : public State {
public:
    TsState(Nanoseconds* counter, TickSource* ts)
        : counter{counter}, ts{ts}, start{ts->getTicks()} {}
    ~TsState() override {
        *counter += ts->ticksTo<Nanoseconds>(ts->getTicks() - start);
    }

private:
    Nanoseconds* counter;
    TickSource* ts;
    TickSource::Tick start;
};

ScopedTimer::ScopedTimer(Nanoseconds* counter, ClockSource* cs)
    : _state{std::make_unique<CsState>(counter, cs)} {}

ScopedTimer::ScopedTimer(Nanoseconds* counter, TickSource* ts)
    : _state{std::make_unique<TsState>(counter, ts)} {}

ScopedTimer::ScopedTimer() = default;
ScopedTimer::ScopedTimer(ScopedTimer&&) noexcept = default;
ScopedTimer& ScopedTimer::operator=(ScopedTimer&&) noexcept = default;
ScopedTimer::~ScopedTimer() = default;

namespace {

/** C++23's `std::to_underlying`. */
constexpr auto toUnderlying(auto e) {
    return static_cast<std::underlying_type_t<decltype(e)>>(e);
}

#define X(e) #e ""_sd,
constexpr std::array sectionNames{MONGO_EXPAND_TIMED_SECTION_IDS(X)};
#undef X

template <typename Dur>
const auto sectionNamesWithDurationSuffix = [] {
    std::string suffix{Dur::mongoUnitSuffix()};
    std::array<std::string, sectionNames.size()> arr{};
    std::transform(sectionNames.begin(), sectionNames.end(), arr.begin(), [&](StringData sec) {
        return std::string{sec} + suffix;
    });
    return arr;
}();

template <typename Dur>
StringData toStringWithDurationSuffix(TimedSectionId id) {
    return sectionNamesWithDurationSuffix<Dur>[toUnderlying(id)];
}

}  // namespace

StringData toString(TimedSectionId id) {
    return sectionNames[toUnderlying(id)];
}

class SectionScopedTimer::State {
public:
    State(BSONObjBuilder* builder, ClockSource* cs, TimedSectionId section)
        : _builder{builder}, _cs{cs}, _section{section}, _beginTime{_cs->now()} {}

    ~State() {
        if (!_builder)
            return;
        _builder->append(toStringWithDurationSuffix<Milliseconds>(_section),
                         durationCount<Milliseconds>(_cs->now() - _beginTime));
    }

private:
    BSONObjBuilder* _builder;
    ClockSource* _cs;
    TimedSectionId _section;
    Date_t _beginTime;
};

SectionScopedTimer::SectionScopedTimer(ClockSource* clockSource,
                                       TimedSectionId section,
                                       BSONObjBuilder* builder)
    : _state{builder ? std::make_unique<State>(builder, clockSource, section)
                     : std::unique_ptr<State>{}} {}

SectionScopedTimer::SectionScopedTimer() = default;
SectionScopedTimer::SectionScopedTimer(SectionScopedTimer&&) noexcept = default;
SectionScopedTimer& SectionScopedTimer::operator=(SectionScopedTimer&&) noexcept = default;
SectionScopedTimer::~SectionScopedTimer() = default;

}  // namespace mongo
