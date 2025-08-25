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
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class DummyMutex {
public:
    void lock();
    void unlock();
};

class ObservableMutexRegistryTest : public unittest::Test {
public:
    struct RegisteredEntry {
        StringData tag;
        observable_mutex_details::ObservationToken* token;

        auto operator<=>(const RegisteredEntry& other) const = default;
    };

    void iterateOnRegistryAndVerify(std::vector<RegisteredEntry> expectedEntries) {
        std::vector<RegisteredEntry> entries;
        registry.iterate(
            [&](StringData tag, const Date_t&, observable_mutex_details::ObservationToken& token) {
                entries.emplace_back(tag, &token);
            });

        // Sort the vectors since the registry does not guarantee order when iterating.
        std::sort(entries.begin(), entries.end());
        std::sort(expectedEntries.begin(), expectedEntries.end());
        ASSERT_EQ(entries, expectedEntries);
    }

    StringData tagA{"A"_sd};
    StringData tagB{"B"_sd};
    ClockSourceMock clk;
    ObservableMutexRegistry registry{&clk};
};

TEST_F(ObservableMutexRegistryTest, EmptyRegistry) {
    iterateOnRegistryAndVerify({});
}

TEST_F(ObservableMutexRegistryTest, AddSingle) {
    ObservableMutex<DummyMutex> m;

    registry.add(tagA, m);

    iterateOnRegistryAndVerify({{tagA, m.token().get()}});
}

TEST_F(ObservableMutexRegistryTest, AddMultiple) {
    ObservableMutex<DummyMutex> one;
    ObservableMutex<DummyMutex> two;
    ObservableMutex<DummyMutex> three;

    registry.add(tagA, one);
    registry.add(tagA, two);
    registry.add(tagB, three);

    iterateOnRegistryAndVerify(
        {{tagA, one.token().get()}, {tagA, two.token().get()}, {tagB, three.token().get()}});
}

TEST_F(ObservableMutexRegistryTest, AddMultipleWithSomeInvalid) {
    ObservableMutex<DummyMutex> one;
    ObservableMutex<DummyMutex> two;
    registry.add(tagA, one);
    registry.add(tagB, two);
    one.token()->invalidate();
    two.token()->invalidate();

    ObservableMutex<DummyMutex> three;
    registry.add(tagA, three);

    ASSERT_EQ(registry.getNumRegistered_forTest(), 3);
    iterateOnRegistryAndVerify(
        {{tagA, one.token().get()}, {tagB, two.token().get()}, {tagA, three.token().get()}});

    // Garbage collection only happens during iteration and after the cb is run.
    ASSERT_EQ(registry.getNumRegistered_forTest(), 1);
    iterateOnRegistryAndVerify({{tagA, three.token().get()}});
}

TEST_F(ObservableMutexRegistryTest, RegistrationTimestamp) {
    ObservableMutex<DummyMutex> one;
    ObservableMutex<DummyMutex> two;
    registry.add(tagA, one);
    clk.advance(Milliseconds(1));
    registry.add(tagB, two);

    StringMap<Date_t> timestamps;
    registry.iterate(
        [&](StringData tag,
            const Date_t& registrationTime,
            observable_mutex_details::ObservationToken&) { timestamps[tag] = registrationTime; });

    ASSERT_LT(timestamps[tagA], timestamps[tagB]);
}

}  // namespace
}  // namespace mongo
