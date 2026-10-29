// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <random>

/**
 * TODO SERVER-114887: Determine if other modules should depend on this.
 */
[[MONGO_MOD_PUBLIC]];
namespace mongo::random_utils {
/**
 * Returns a random number generator that is a static object initialized once per thread.
 */
PseudoRandom& getRNG();

/**
 * Helper for generating pseudo-random order of vector *in tests* in place of std::shuffle for
 * consistent, platform-independent results.
 *
 * Warning: The implementation below uses modulo as a simple, platform-independent way to get a
 * close-to-uniform distribution.
 */
class PseudoRandomGenerator {
public:
    PseudoRandomGenerator(int seed) {
        gen = std::mt19937(seed);
    }

    template <typename T>
    void shuffleVector(std::vector<T>& vec) {
        // Implement Fisher-Yates shuffle manually to make the algorithm deterministic as
        // std::shuffle has platform-specific implementations.
        for (std::size_t i = vec.size(); i > 1; --i) {
            std::size_t j = (gen() + 1) % i;
            std::swap(vec[i - 1], vec[j]);
        }
    }

    // Helper to generate integer values randomly distributed in the range [min, max].
    auto generateUniformInt(int min, int max) {
        return (gen() % (max - min + 1)) + min;
    }

    // Helper to generate a random boolean (like a coin flip).
    bool generateRandomBool() {
        return generateUniformInt(0, 1) == 0;
    }

private:
    std::mt19937 gen;
};

}  // namespace mongo::random_utils
