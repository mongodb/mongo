// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/deferred.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
using std::string;
using namespace std::string_literals;


namespace {
// Type that counts destructions so we can verify the cached value is destroyed
// when move-assigning an uninitialized Deferred over an initialized one.
struct DestructionCounter {
    static int destructorCount;
    ~DestructionCounter() {
        ++destructorCount;
    }
};
int DestructionCounter::destructorCount = 0;

DestructionCounter makeDestructionCounter() {
    return DestructionCounter{};
}
}  // namespace

TEST(DeferredTest, EagerInitialization) {
    // Note that when initialised with a value eagerly, Deferred cannot
    // deduce the initialiser type to store (as one has not been provided).
    Deferred<string (*)()> eager{"someString"};
    ASSERT_TRUE(eager.isInitialized());
    ASSERT_EQ(eager.get(), "someString"s);
    ASSERT_EQ(*eager, "someString"s);
}

TEST(DeferredTest, DeferredInitialization) {
    size_t initializationCount = 0;
    Deferred deferred{[&]() {
        initializationCount++;
        return "someString"s;
    }};
    ASSERT_FALSE(deferred.isInitialized());

    // Ensure the deferred object wasn't initialized on creation.
    ASSERT_EQ(initializationCount, 0);

    // Ensure that the deferred object is initialized on pointer dereferences.
    ASSERT_FALSE(deferred->empty());
    ASSERT_TRUE(deferred.isInitialized());

    ASSERT_EQ(initializationCount, 1);

    // Ensure that the content of the deferred object is equal to its raw counterpart, while also
    // verifing that it is initialized at most once.
    ASSERT_EQ(deferred.get(), "someString"s);
    ASSERT_EQ(initializationCount, 1);
}

TEST(DeferredTest, DeferredInitializationWithOneArgument) {
    size_t initializationCount = 0;
    Deferred deferred{[&](const string& input) {
        initializationCount++;
        return "{" + input + "}";
    }};

    // Ensure the deferred object wasn't initialized on creation.
    ASSERT_EQ(initializationCount, 0);

    // Ensure that the content of the deferred object is equal to its raw counterpart, while also
    // verifing that it is initialized at most once.
    ASSERT_EQ(deferred.get("more curlies"), "{more curlies}"s);
    ASSERT_EQ(initializationCount, 1);

    // Note that the value is cached, so it's not really valid to call it with a different argument.
    ASSERT_EQ(deferred.get("merganser"), "{more curlies}"s);
    ASSERT_EQ(initializationCount, 1);
}

TEST(DeferredTest, DeferredInitializationWithTwoArgs) {
    Deferred deferred{[&](const string& input, const string& prefix) {
        return prefix + input;
    }};

    ASSERT_EQ(deferred.get("cowbell", "more "), "more cowbell"s);
    ASSERT_EQ(deferred.get("cowbell", "more "), "more cowbell"s);
    ASSERT_EQ(deferred.get("cowbell", "less?"), "more cowbell"s);
    ASSERT_EQ(deferred.get("tests", "better"), "more cowbell"s);
}

TEST(DeferredTest, MoveAssignUninitializedOverInitializedDestroysOldValue) {
    DestructionCounter::destructorCount = 0;

    Deferred<DestructionCounter (*)()> initialized{&makeDestructionCounter};
    ASSERT_FALSE(initialized.isInitialized());

    initialized.get();
    ASSERT_TRUE(initialized.isInitialized());
    ASSERT_EQ(DestructionCounter::destructorCount, 0);

    initialized = Deferred<DestructionCounter (*)()>(&makeDestructionCounter);

    ASSERT_EQ(DestructionCounter::destructorCount, 1)
        << "Expected the old cached value to be destroyed exactly once";
    ASSERT_FALSE(initialized.isInitialized())
        << "After assigning from uninitialized, the Deferred should be uninitialized";
}
}  // namespace mongo
