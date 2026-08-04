// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/op_observer_noop.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


std::vector<std::span<const char>> toSpans(const std::vector<std::string>& strings) {
    std::vector<std::span<const char>> spans;
    spans.reserve(strings.size());
    for (const auto& s : strings) {
        spans.push_back(s);
    }
    return spans;
}

std::vector<std::string> toStrings(std::span<const std::span<const char>> spans) {
    std::vector<std::string> strings;
    strings.reserve(spans.size());
    for (auto s : spans) {
        strings.emplace_back(s.begin(), s.end());
    }
    return strings;
}

/**
 * Records only the scalar (single key, single value) container inserts. The batched overloads are
 * left to OpObserverNoop, so this observer observes the inherited fan-out: one recorded scalar
 * insert per key that the batch was expanded into.
 */
class ScalarRecordingObserver : public OpObserverNoop {
public:
    // Declaring the scalar overloads below would otherwise hide the batched overloads inherited
    // from OpObserverNoop, making them unreachable by name lookup on this type.
    using OpObserverNoop::onContainerInsert;

    struct ScalarInsert {
        std::string ident;
        // Exactly one of these is set, recording which of the two scalar overloads fired.
        boost::optional<int64_t> intKey;
        boost::optional<std::string> bytesKey;
        std::string value;
    };

    void onContainerInsert(OperationContext*,
                           std::string_view ident,
                           int64_t key,
                           std::span<const char> value) override {
        scalarInserts.push_back(
            {std::string{ident}, key, boost::none, std::string{value.begin(), value.end()}});
    }

    void onContainerInsert(OperationContext*,
                           std::string_view ident,
                           std::span<const char> key,
                           std::span<const char> value) override {
        scalarInserts.push_back({std::string{ident},
                                 boost::none,
                                 std::string{key.begin(), key.end()},
                                 std::string{value.begin(), value.end()}});
    }

    std::vector<ScalarInsert> scalarInserts;
};

class OpObserverNoopTest : public ServiceContextTest {
protected:
    OperationContext* opCtx() {
        return _opCtxHolder.get();
    }

    static constexpr auto kIdent = "testIdent"sv;

private:
    ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
};

// A contiguous range insert fans out into one scalar insert per value, with keys derived as
// base + i.
TEST_F(OpObserverNoopTest, RangeInsertFansOutToScalarInsertsWithDerivedKeys) {
    const std::vector<std::string> values{"a", "bb", "ccc"};
    constexpr int64_t kBase = 100;

    ScalarRecordingObserver observer;
    // Dispatch through the base interface, as production callers do.
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, kBase, toSpans(values));

    ASSERT_EQ(observer.scalarInserts.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        const auto& insert = observer.scalarInserts[i];
        EXPECT_EQ(insert.ident, kIdent);
        EXPECT_TRUE(insert.intKey.has_value());
        EXPECT_EQ(*insert.intKey, kBase + static_cast<int64_t>(i));
        EXPECT_FALSE(insert.bytesKey.has_value());
        EXPECT_EQ(insert.value, values[i]);
    }
}

// A multi-key insert fans out into one scalar insert per key, all sharing the single value.
TEST_F(OpObserverNoopTest, MultiKeyInsertFansOutToScalarInsertsSharingValue) {
    const std::vector<std::string> keyStrings{"k0", "k1", "k2"};
    constexpr std::string_view value = "sharedValue";

    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, toSpans(keyStrings), value);

    ASSERT_EQ(observer.scalarInserts.size(), keyStrings.size());
    for (size_t i = 0; i < keyStrings.size(); ++i) {
        const auto& insert = observer.scalarInserts[i];
        EXPECT_EQ(insert.ident, kIdent);
        EXPECT_TRUE(insert.bytesKey.has_value());
        EXPECT_EQ(*insert.bytesKey, keyStrings[i]);
        EXPECT_FALSE(insert.intKey.has_value());
        EXPECT_EQ(insert.value, value);
    }
}

TEST_F(OpObserverNoopTest, RangeInsertWithNoValuesDoesNothing) {
    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, 100, std::span<const std::span<const char>>{});

    ASSERT_TRUE(observer.scalarInserts.empty());
}

TEST_F(OpObserverNoopTest, MultiKeyInsertWithNoKeysDoesNothing) {
    const std::string value = "unused";

    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, std::span<const std::span<const char>>{}, value);

    ASSERT_TRUE(observer.scalarInserts.empty());
}

// A single value is in range at INT64_MAX, since the highest derived key is base + 0.
TEST_F(OpObserverNoopTest, RangeInsertOfOneValueAtMaxKeyDoesNotOverflow) {
    const std::vector<std::string> values{"a"};
    constexpr auto kBase = std::numeric_limits<int64_t>::max();

    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, kBase, toSpans(values));

    ASSERT_EQ(observer.scalarInserts.size(), 1u);
    ASSERT_EQ(*observer.scalarInserts[0].intKey, kBase);
}

// A second value would derive a key past INT64_MAX, which must be rejected before any insert is
// observed rather than silently wrapping around.
TEST_F(OpObserverNoopTest, RangeInsertThrowsWhenDerivedKeyOverflows) {
    const std::vector<std::string> values{"a", "bb"};
    constexpr auto kBase = std::numeric_limits<int64_t>::max();

    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    ASSERT_THROWS_CODE(opObserver.onContainerInsert(opCtx(), kIdent, kBase, toSpans(values)),
                       DBException,
                       13064500);

    ASSERT_TRUE(observer.scalarInserts.empty());
}

// A single-key batch is equivalent to one plain single-key insert, not to an array-shaped one.
TEST_F(OpObserverNoopTest, MultiKeyInsertOfOneKeyFansOutToOneScalarInsert) {
    const std::vector<std::string> keyStrings{"onlyKey"};
    constexpr std::string_view value = "v";

    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, toSpans(keyStrings), value);

    ASSERT_EQ(observer.scalarInserts.size(), 1u);
    EXPECT_EQ(*observer.scalarInserts[0].bytesKey, keyStrings[0]);
    EXPECT_EQ(observer.scalarInserts[0].value, value);
}

TEST_F(OpObserverNoopTest, RangeInsertOfOneValueFansOutToOneScalarInsert) {
    const std::vector<std::string> values{"onlyValue"};
    constexpr int64_t kBase = 7;

    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, kBase, toSpans(values));

    ASSERT_EQ(observer.scalarInserts.size(), 1u);
    EXPECT_EQ(*observer.scalarInserts[0].intKey, kBase);
    EXPECT_EQ(observer.scalarInserts[0].value, values[0]);
}

// Values are matched to keys positionally, so a repeated value must not collapse or reorder the
// derived keys.
TEST_F(OpObserverNoopTest, RangeInsertWithRepeatedValuesKeepsPositionalKeyMapping) {
    const std::vector<std::string> values{"dup", "other", "dup"};
    constexpr int64_t kBase = -1;

    ScalarRecordingObserver observer;
    OpObserver& opObserver = observer;
    opObserver.onContainerInsert(opCtx(), kIdent, kBase, toSpans(values));

    ASSERT_EQ(observer.scalarInserts.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(*observer.scalarInserts[i].intKey, kBase + static_cast<int64_t>(i));
        EXPECT_EQ(observer.scalarInserts[i].value, values[i]);
    }
}

}  // namespace
}  // namespace mongo
