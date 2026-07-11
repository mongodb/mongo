// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/versioned_value.h"

#include "mongo/platform/atomic.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

class VersionedValueTest : public unittest::Test {
public:
    VersionedValueTest() {
        update();
    }

    const auto& value() const {
        return _value;
    }

    void update() {
        _value.update(std::make_shared<int>(_counter++));
    }

private:
    int _counter;
    VersionedValue<int> _value;
};

TEST_F(VersionedValueTest, SnapshotIsNotStaleIfValueHasNotChanged) {
    auto snapshot = value().makeSnapshot();
    ASSERT_TRUE(value().isCurrent(snapshot));
}

TEST_F(VersionedValueTest, SnapshotIsStaleIfValueHasChanged) {
    auto snapshot = value().makeSnapshot();
    update();
    ASSERT_FALSE(value().isCurrent(snapshot));
}

TEST_F(VersionedValueTest, MakeSnapshotDoesNotChangeVersionIfValueHasNotChanged) {
    auto snapshot1 = value().makeSnapshot();
    auto snapshot2 = value().makeSnapshot();

    ASSERT_EQ(snapshot1.version(), snapshot2.version());
    ASSERT_EQ(*snapshot1, *snapshot2);
}

TEST_F(VersionedValueTest, MakeSnapshotRetrievesNewVersionAfterUpdate) {
    auto snapshot1 = value().makeSnapshot();
    update();
    auto snapshot2 = value().makeSnapshot();
    ASSERT_EQ(snapshot2.version(), snapshot1.version() + 1);
    ASSERT_EQ(*snapshot2, *snapshot1 + 1);
}

TEST_F(VersionedValueTest, SnapshotRemainsValidAfterUpdate) {
    auto snapshot = value().makeSnapshot();
    const auto& valuePtr = value().getValue_forTest();
    ASSERT_EQ(valuePtr.use_count(), 2);
    update();
    ASSERT_EQ(valuePtr.use_count(), 1);
}

using VersionedValueTestDeathTest = VersionedValueTest;
DEATH_TEST_F(VersionedValueTestDeathTest, CannotDereferenceUninitializedValue, "invariant") {
    VersionedValue<bool> value;
    *value.makeSnapshot();
}

}  // namespace
}  // namespace mongo
