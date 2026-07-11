// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/split_timer.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

#include <array>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

namespace m = unittest::match;

/** Match that `x` converts to `true` and that `*x` matches `m`. */
MATCHER_P(WhenDereferenced, m, "") {
    if (!arg)
        return false;
    return ExplainMatchResult(m, *arg, result_listener);
}

/** Match that `static_cast<bool>(x)` matches `m`. */
MATCHER_P(WhenBool, m, "") {
    return ExplainMatchResult(m, static_cast<bool>(arg), result_listener);
}

enum class SomeTimeSplitId : size_t {
    a,
    b,
    c,
};

enum class SomeIntervalId : size_t {
    ab,
    ac,
};

struct IDef {
    SomeIntervalId iId;
    std::string_view name;
    SomeTimeSplitId start;
    SomeTimeSplitId end;
};

static constexpr auto iDefs = std::array{
    IDef{SomeIntervalId::ab, "abMillis"sv, SomeTimeSplitId::a, SomeTimeSplitId::b},
    IDef{SomeIntervalId::ac, "acMillis"sv, SomeTimeSplitId::a, SomeTimeSplitId::c},
};

struct SomePolicy {
    using TimeSplitIdType = SomeTimeSplitId;
    using IntervalIdType = SomeIntervalId;

    static constexpr size_t numTimeSplitIds = 3;
    static constexpr size_t numIntervalIds = 2;

    static constexpr size_t toIdx(TimeSplitIdType e) {
        return static_cast<size_t>(e);
    }
    static constexpr size_t toIdx(IntervalIdType e) {
        return static_cast<size_t>(e);
    }

    static constexpr std::string_view getName(TimeSplitIdType e) {
        constexpr auto arr = std::array{"a"sv, "b"sv, "c"sv};
        return arr[toIdx(e)];
    }
    static constexpr std::string_view getName(IntervalIdType e) {
        return iDefs[toIdx(e)].name;
    }

    static constexpr TimeSplitIdType getStartSplit(IntervalIdType e) {
        return iDefs[toIdx(e)].start;
    }
    static constexpr TimeSplitIdType getEndSplit(IntervalIdType e) {
        return iDefs[toIdx(e)].end;
    }

    void onStart(SplitTimer<SomePolicy>* t) {
        mockOnStart(t);
    }
    void onFinish(SplitTimer<SomePolicy>* t) {
        mockOnFinish(t);
    }

    Timer makeTimer() {
        return Timer{clock};
    }

    std::function<void(SplitTimer<SomePolicy>*)> mockOnStart;
    std::function<void(SplitTimer<SomePolicy>*)> mockOnFinish;
    TickSource* clock;
};

class SplitTimerTest : public unittest::Test {
public:
    SplitTimerTest() {
        policy.mockOnStart = [&](auto&&...) {
            ++starts;
        };
        policy.mockOnFinish = [&](auto&&...) {
            ++finishes;
        };
        policy.clock = &clock;
    }

    template <typename Pol>
    BSONObj makeSplitTimerReport(const SplitTimer<Pol>& splitTimer) {
        BSONObjBuilder bob;
        splitTimer.appendIntervals(bob);
        return bob.obj();
    };

    int starts = 0;
    int finishes = 0;
    TickSourceMock<Milliseconds> clock;
    SomePolicy policy;
};

TEST_F(SplitTimerTest, BasicGetSplitInterval) {
    struct Trial {
        Milliseconds ab;
        Milliseconds ac;
        Milliseconds err = Milliseconds{5};
    };
    static constexpr auto trials = std::array{
        Trial{Milliseconds{0}, Milliseconds{0}},
        Trial{Milliseconds{0}, Milliseconds{10}},
        Trial{Milliseconds{10}, Milliseconds{30}},
    };
    auto matchDerefRange = [](auto lo, auto hi) {
        return WhenDereferenced(m::AllOf(m::Ge(lo), m::Le(hi)));
    };

    for (auto&& [ab, ac, err] : trials) {
        starts = 0;
        finishes = 0;
        {
            SplitTimer t{policy};
            ASSERT_EQ(starts, 1);
            ASSERT_THAT(t.getSplitInterval(SomeIntervalId::ab), WhenBool(m::Eq(false)));
            t.notify(SomeTimeSplitId::a);

            clock.advance(ab);
            t.notify(SomeTimeSplitId::b);
            ASSERT_THAT(t.getSplitInterval(SomeIntervalId::ab),
                        matchDerefRange(ab - err, ab + err));
            ASSERT_THAT(t.getSplitInterval(SomeIntervalId::ac), WhenBool(m::Eq(false)));

            clock.advance(ac - ab);
            t.notify(SomeTimeSplitId::c);
            ASSERT_THAT(t.getSplitInterval(SomeIntervalId::ac),
                        matchDerefRange(ac - err, ac + err));

            ASSERT_EQ(finishes, 0);
        }
        ASSERT_EQ(finishes, 1);
    }
}

TEST_F(SplitTimerTest, BSONFormatting) {
    SplitTimer t{policy};
    t.notify(SomeTimeSplitId::a);
    clock.advance(Milliseconds{10});
    t.notify(SomeTimeSplitId::b);
    clock.advance(Milliseconds{20});
    t.notify(SomeTimeSplitId::c);
    ASSERT_BSONOBJ_EQ(makeSplitTimerReport(t),
                      BSONObjBuilder{}.append("abMillis", 10).append("acMillis", 30).obj());
}

}  // namespace
}  // namespace mongo
