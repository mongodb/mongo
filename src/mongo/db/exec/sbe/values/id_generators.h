/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/slot.h"

namespace mongo::sbe::value {
/**
 * A reusable id generator suitable for use with integer ids that generates each new id by adding an
 * increment to the previously generated id. This generator is not thread safe; calls to
 * generateByIncrementing must be serialized.
 */
template <class T>
class IncrementingIdGenerator {
protected:
    /**
     * Constructs a new generator using 'startingId' as the first generated id and 'incrementStep'
     * as the value to add to generate subsequent ids. Note that 'incrementStep' may be negative but
     * must not be zero.
     */
    IncrementingIdGenerator(T startingId, T incrementStep)
        : _currentId(startingId), _incrementStep(incrementStep) {}

    T generateByIncrementing() {
        _currentId += _incrementStep;
        return _currentId;
    }

private:
    T _currentId;
    T _incrementStep;
};

template <class T>
class IdGenerator : IncrementingIdGenerator<T> {
public:
    IdGenerator(T startingId = 0, T incrementStep = 1)
        : IncrementingIdGenerator<T>(startingId, incrementStep) {}

    T generate() {
        return this->generateByIncrementing();
    }

    auto generateMultiple(size_t num) {
        std::vector<T> idVector(num);
        for (T& id : idVector) {
            id = generate();
        }
        return idVector;
    }
};

using SlotIdGenerator = IdGenerator<value::SlotId>;
using FrameIdGenerator = IdGenerator<FrameId>;
using SpoolIdGenerator = IdGenerator<SpoolId>;
}  // namespace mongo::sbe::value
