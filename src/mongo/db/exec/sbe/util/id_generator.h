// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
/**
 * A reusable id generator suitable for use with integer ids that generates each new id by adding an
 * increment to the previously generated id. This generator is not thread safe; calls to
 * generateByIncrementing must be serialized.
 */
template <class T, T IncrementStep = 1>
class IncrementingIdGenerator {
protected:
    /**
     * Constructs a new generator using 'startingId' as the first generated id.
     */
    IncrementingIdGenerator(T startingId) : _currentId(startingId) {}

    T generateByIncrementing() {
        _currentId += IncrementStep;
        return _currentId;
    }

private:
    T _currentId;
};

template <class T, class Container = std::vector<T>, T IncrementStep = 1>
class IdGenerator : IncrementingIdGenerator<T, IncrementStep> {
public:
    IdGenerator(T startingId = 0) : IncrementingIdGenerator<T, IncrementStep>(startingId) {}

    T generate() {
        return this->generateByIncrementing();
    }

    auto generateMultiple(size_t num) {
        Container idVector(num);
        for (T& id : idVector) {
            id = generate();
        }
        return idVector;
    }
};
}  // namespace mongo
