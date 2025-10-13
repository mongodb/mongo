/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/platform/random.h"

namespace mongo::random_utils {
/**
 * Returns a random number generator that is a static object initialized once per thread.
 */
PseudoRandom& getRNG();

/**
 * Helper for generating pseudo-random order of vector.
 *
 * For example, used in testing with a fixed seed to ensure a consistent random order that is
 * platform-independent.
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

private:
    std::mt19937 gen;
};

}  // namespace mongo::random_utils
