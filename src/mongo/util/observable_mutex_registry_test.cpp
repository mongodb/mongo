/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/observable_mutex_registry.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class DummyMutex {
public:
    void lock();
    void unlock();
};

class ObservableMutexRegistryTest : public unittest::Test {
public:
    using TrackingMode = ObservableMutexRegistry::TrackingMode;

    struct RegisteredEntry {
        StringData tag;
        observable_mutex_details::ObservationToken* token;
        TrackingMode mode;

        bool operator==(const RegisteredEntry& other) const = default;
    };

    void iterateOnRegistryAndVerify(std::vector<RegisteredEntry> expectedEntries) {
        std::vector<RegisteredEntry> entries;
        registry.iterate([&](StringData tag,
                             TrackingMode mode,
                             observable_mutex_details::ObservationToken& token) {
            entries.emplace_back(tag, &token, mode);
        });
        ASSERT_EQ(entries, expectedEntries);
    }

    StringData tag{"dummy-tag"_sd};
    ObservableMutexRegistry registry;
};

TEST_F(ObservableMutexRegistryTest, EmptyRegistry) {
    iterateOnRegistryAndVerify({});
}

TEST_F(ObservableMutexRegistryTest, AggregateNew) {
    ObservableMutex<DummyMutex> m;

    registry.add(tag, m);

    iterateOnRegistryAndVerify({{tag, m.token().get(), TrackingMode::kAggregate}});
}

TEST_F(ObservableMutexRegistryTest, SeparateNew) {
    ObservableMutex<DummyMutex> m;

    registry.add(tag, m, TrackingMode::kSeparate);

    iterateOnRegistryAndVerify({{tag, m.token().get(), TrackingMode::kSeparate}});
}

TEST_F(ObservableMutexRegistryTest, AggregateReused) {
    ObservableMutex<DummyMutex> one;
    ObservableMutex<DummyMutex> two;

    registry.add(tag, one);
    registry.add(tag, two);

    iterateOnRegistryAndVerify({{tag, one.token().get(), TrackingMode::kAggregate},
                                {tag, two.token().get(), TrackingMode::kAggregate}});
}

TEST_F(ObservableMutexRegistryTest, SeparateReusedWhenOldValid) {
    ObservableMutex<DummyMutex> one;
    ObservableMutex<DummyMutex> two;

    registry.add(tag, one, TrackingMode::kSeparate);
    ASSERT_THROWS_CODE_AND_WHAT(
        registry.add(tag, two, TrackingMode::kSeparate),
        DBException,
        ErrorCodes::InternalError,
        "Unable to register more than one mutex with separate tracking mode");

    iterateOnRegistryAndVerify({{tag, one.token().get(), TrackingMode::kSeparate}});
}

TEST_F(ObservableMutexRegistryTest, SeparateReusedWhenOldInvalid) {
    {
        ObservableMutex<DummyMutex> one;
        registry.add(tag, one, TrackingMode::kSeparate);
        iterateOnRegistryAndVerify({{tag, one.token().get(), TrackingMode::kSeparate}});
    };

    ObservableMutex<DummyMutex> two;
    registry.add(tag, two, TrackingMode::kSeparate);
    iterateOnRegistryAndVerify({{tag, two.token().get(), TrackingMode::kSeparate}});
}

TEST_F(ObservableMutexRegistryTest, SameTagUnderDifferentModesSepFirst) {
    ObservableMutex<DummyMutex> one;
    ObservableMutex<DummyMutex> two;

    registry.add(tag, one, TrackingMode::kSeparate);
    iterateOnRegistryAndVerify({{tag, one.token().get(), TrackingMode::kSeparate}});

    ASSERT_THROWS_CODE_AND_WHAT(registry.add(tag, two, TrackingMode::kAggregate),
                                DBException,
                                ErrorCodes::InternalError,
                                "Unable to register the same tag under different tracking modes");
}

TEST_F(ObservableMutexRegistryTest, SameTagUnderDifferentModesAggFirst) {
    ObservableMutex<DummyMutex> one;
    ObservableMutex<DummyMutex> two;

    registry.add(tag, one, TrackingMode::kAggregate);
    iterateOnRegistryAndVerify({{tag, one.token().get(), TrackingMode::kAggregate}});

    ASSERT_THROWS_CODE_AND_WHAT(registry.add(tag, two, TrackingMode::kSeparate),
                                DBException,
                                ErrorCodes::InternalError,
                                "Unable to register the same tag under different tracking modes");
}

}  // namespace
}  // namespace mongo
