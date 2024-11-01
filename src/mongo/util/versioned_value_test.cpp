/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/versioned_value.h"

#include <memory>

#include "mongo/platform/atomic.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

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

DEATH_TEST_F(VersionedValueTest, CannotDereferenceUninitializedValue, "invariant") {
    VersionedValue<bool> value;
    *value.makeSnapshot();
}

}  // namespace
}  // namespace mongo
