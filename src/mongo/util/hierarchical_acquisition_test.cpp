/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/util/hierarchical_acquisition.h"

#include <fmt/format.h>

#include "mongo/platform/source_location.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace hierarchical_acquisition_detail;

struct Context {
    friend std::string toString(Context context) {
        using namespace fmt::literals;
        return R"({{"level": {}, "sourceLocation": "{}"}})"_format(context.index,
                                                                   context.loc.toString());
    }

    friend std::ostream& operator<<(std::ostream& os, const Context& context) {
        return os << toString(context);
    }

    Level::IndexType index;
    SourceLocationHolder loc;
};

#define MONGO_MAKE_CONTEXT(index)      \
    Context {                          \
        index, MONGO_SOURCE_LOCATION() \
    }

class HierarchicalAcquisitionTest : public unittest::Test {
public:
    void checkAdd(Level level, Set::AddResult expectedResult, Context context) {
        ASSERT_EQ(_set.add(level), expectedResult) << context;
        ASSERT_TRUE(_set.has(level)) << context;
    }

    void checkRemove(Level level, Set::RemoveResult expectedResult, Context context) {
        if (expectedResult != Set::RemoveResult::kInvalidWasAbsent) {
            ASSERT_TRUE(_set.has(level)) << context;
        } else {
            ASSERT_FALSE(_set.has(level)) << context;
        }
        ASSERT_EQ(_set.remove(level), expectedResult) << context;
        ASSERT_FALSE(_set.has(level)) << context;
    }

    void resetSet() {
        _set = Set();
    }

private:
    Set _set;
};

TEST_F(HierarchicalAcquisitionTest, AcquireRelease) {
    // This test performs the simplest idempotent set of successful operations on a single level L1:
    // - add(L1) suceeds because nothing is set
    // - remove(L1) suceeds because only L1 is set

    for (auto i = Level::kMinIndex; i <= Level::kMaxIndex; ++i) {
        Level level(i);

        checkAdd(level, Set::AddResult::kValidWasAbsent, MONGO_MAKE_CONTEXT(i));
        checkRemove(level, Set::RemoveResult::kValidWasPresent, MONGO_MAKE_CONTEXT(i));
    };
}

TEST_F(HierarchicalAcquisitionTest, ReleaseAcquireAcquireReleaseRelease) {
    // This test performs an exhaustive idempotent set of operations on a single Level L1:
    // - remove(L1) fails because nothing is set
    // - add(L1) suceeds because nothing is set
    // - add(L1) fails because L1 is set
    // - remove(L1) suceeds because L1 is set
    // - remove(L1) fails because nothing is set

    for (auto i = Level::kMinIndex; i <= Level::kMaxIndex; ++i) {
        Level level(i);

        // Removing an empty level fails
        checkRemove(Level(i), Set::RemoveResult::kInvalidWasAbsent, MONGO_MAKE_CONTEXT(i));

        // Adding an empty level succeeds
        checkAdd(Level(i), Set::AddResult::kValidWasAbsent, MONGO_MAKE_CONTEXT(i));

        // Adding a full level fails
        checkAdd(Level(i), Set::AddResult::kInvalidWasPresent, MONGO_MAKE_CONTEXT(i));

        // Removing a full level succeeds
        checkRemove(Level(i), Set::RemoveResult::kValidWasPresent, MONGO_MAKE_CONTEXT(i));

        // Removing an empty level again fails
        checkRemove(Level(i), Set::RemoveResult::kInvalidWasAbsent, MONGO_MAKE_CONTEXT(i));
    };
}

TEST_F(HierarchicalAcquisitionTest, DescendingAcquireLagRelease) {
    // This test verifies that interleaved acquire releases fail as expected for two levels L1 and
    // L2 where L1 > L2:
    // - add(L2) suceeds because L1 > L2
    // - remove(L1) fails because L1 > L2

    auto testWithSkip = [&](Level::IndexType skip) {
        // Set the first level
        auto lag = Level::kMaxIndex;
        checkAdd(Level(lag), Set::AddResult::kValidWasAbsent, MONGO_MAKE_CONTEXT(lag));

        for (auto i = lag - skip; i >= Level::kMinIndex; lag = i, i -= skip) {
            // Adding the current level succeeds
            checkAdd(Level(i), Set::AddResult::kValidWasAbsent, MONGO_MAKE_CONTEXT(i));

            // Removing the lag level fails
            checkRemove(Level(lag), Set::RemoveResult::kInvalidWasPresent, MONGO_MAKE_CONTEXT(lag));
        }

        resetSet();
    };

    testWithSkip(1);
    testWithSkip(2);
    testWithSkip(3);
    testWithSkip(8);
}

TEST_F(HierarchicalAcquisitionTest, AscendingAcquireReleaseLag) {
    // This test verifies that interleaved acquire releases fail as expected for two levels L1 and
    // L2 where L1 < L2:
    // - add(L2) fails because L1 < L2
    // - remove(L1) suceeds because L1 < L2

    auto testWithSkip = [&](Level::IndexType skip) {
        // Set the first level
        auto lag = Level::kMinIndex;
        checkAdd(Level(lag), Set::AddResult::kValidWasAbsent, MONGO_MAKE_CONTEXT(lag));

        for (auto i = lag + skip; i <= Level::kMaxIndex; lag = i, i += skip) {
            // Adding the current level fails
            checkAdd(Level(i), Set::AddResult::kInvalidWasAbsent, MONGO_MAKE_CONTEXT(i));

            // Removing the lag level succeeds
            checkRemove(Level(lag), Set::RemoveResult::kValidWasPresent, MONGO_MAKE_CONTEXT(lag));
        }

        resetSet();
    };

    testWithSkip(1);
    testWithSkip(2);
    testWithSkip(3);
    testWithSkip(8);
}

TEST_F(HierarchicalAcquisitionTest, DescendingAcquireAscendingRelease) {
    // This test verifies that the expected set of sequential operations succeed:
    // - add(L) for every L such that every previous L(n-1) > L
    // - remove(L) for every L such that every previous L(n-1) < L

    auto testWithSkip = [&](Level::IndexType skip) {
        auto i = Level::kMaxIndex;
        for (; i >= Level::kMinIndex; i -= skip) {
            // Adding the current level succeeds
            checkAdd(Level(i), Set::AddResult::kValidWasAbsent, MONGO_MAKE_CONTEXT(i));
        };

        for (i += skip; i <= Level::kMaxIndex; i += skip) {
            // Removing the current level succeeds
            checkRemove(Level(i), Set::RemoveResult::kValidWasPresent, MONGO_MAKE_CONTEXT(i));
        };

        resetSet();
    };

    testWithSkip(1);
    testWithSkip(2);
    testWithSkip(3);
    testWithSkip(8);
}

TEST_F(HierarchicalAcquisitionTest, AscendingAcquireDescendingRelease) {
    // This test verifies that the expected set of sequential operations fail:
    // - add(L) for every L such that every previous L(n-1) < L
    // - remove(L) for every L such that every previous L(n-1) > L

    auto testWithSkip = [&](Level::IndexType skip) {
        auto i = Level::kMinIndex;

        // Set the first level
        checkAdd(Level(i), Set::AddResult::kValidWasAbsent, MONGO_MAKE_CONTEXT(i));

        for (i += skip; i <= (Level::kMaxIndex); i += skip) {
            // Adding the current level fails
            checkAdd(Level(i), Set::AddResult::kInvalidWasAbsent, MONGO_MAKE_CONTEXT(i));
        };


        for (i -= skip; i > Level::kMinIndex; i -= skip) {
            // Removing the current level fails
            checkRemove(Level(i), Set::RemoveResult::kInvalidWasPresent, MONGO_MAKE_CONTEXT(i));
        };

        // firstLevel is the only Level, so it succeeds
        checkRemove(Level(i), Set::RemoveResult::kValidWasPresent, MONGO_MAKE_CONTEXT(i));

        resetSet();
    };

    testWithSkip(1);
    testWithSkip(2);
    testWithSkip(3);
    testWithSkip(8);
}

}  // namespace
}  // namespace mongo
