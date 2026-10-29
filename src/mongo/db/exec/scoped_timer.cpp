// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/scoped_timer.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
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
using namespace std::literals::string_view_literals;

/** C++23's `std::to_underlying`. */
constexpr auto toUnderlying(auto e) {
    return static_cast<std::underlying_type_t<decltype(e)>>(e);
}

#define X(e) #e ""sv,
constexpr std::array sectionNames{MONGO_EXPAND_TIMED_SECTION_IDS(X)};
#undef X

template <typename Dur>
const auto sectionNamesWithDurationSuffix = [] {
    std::string suffix{Dur::mongoUnitSuffix()};
    std::array<std::string, sectionNames.size()> arr{};
    std::transform(sectionNames.begin(),
                   sectionNames.end(),
                   arr.begin(),
                   [&](std::string_view sec) { return std::string{sec} + suffix; });
    return arr;
}();

template <typename Dur>
std::string_view toStringWithDurationSuffix(TimedSectionId id) {
    return sectionNamesWithDurationSuffix<Dur>[toUnderlying(id)];
}

}  // namespace

std::string_view toString(TimedSectionId id) {
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
