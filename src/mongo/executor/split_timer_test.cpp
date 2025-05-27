/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/executor/split_timer.h"

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

#include <array>
#include <string>

#include <fmt/format.h>

namespace mongo {
namespace {

namespace m = unittest::match;

/** Match that `x` converts to `true` and that `*x` matches `m`. */
template <typename M>
class WhenDereferenced : public m::Matcher {
public:
    explicit WhenDereferenced(M&& m) : _m{std::move(m)} {}

    std::string describe() const {
        return fmt::format("WhenDereferenced({})", _m.describe());
    }

    template <typename X>
    m::MatchResult match(const X& x) const {
        if (!x)
            return {false, "converts to a false bool value"};
        return _m.match(*x);
    }

private:
    M _m;
};

/** Match that `static_cast<bool>(x)` matches `m`. */
template <typename M>
class WhenBool : public m::Matcher {
public:
    explicit WhenBool(M&& m) : _m{std::move(m)} {}

    std::string describe() const {
        return fmt::format("WhenBool({})", _m.describe());
    }

    template <typename X>
    m::MatchResult match(const X& x) const {
        return _m.match(static_cast<bool>(x));
    }

private:
    M _m;
};

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
    StringData name;
    SomeTimeSplitId start;
    SomeTimeSplitId end;
};

static constexpr auto iDefs = std::array{
    IDef{SomeIntervalId::ab, "abMillis"_sd, SomeTimeSplitId::a, SomeTimeSplitId::b},
    IDef{SomeIntervalId::ac, "acMillis"_sd, SomeTimeSplitId::a, SomeTimeSplitId::c},
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

    static constexpr StringData getName(TimeSplitIdType e) {
        constexpr auto arr = std::array{"a"_sd, "b"_sd, "c"_sd};
        return arr[toIdx(e)];
    }
    static constexpr StringData getName(IntervalIdType e) {
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
