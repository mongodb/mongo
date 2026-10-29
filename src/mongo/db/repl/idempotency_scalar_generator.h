// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/platform/random.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class Value;

class ScalarGenerator {
public:
    virtual ~ScalarGenerator() = default;
    virtual Value generateScalar() const = 0;
};

class TrivialScalarGenerator : public ScalarGenerator {
public:
    TrivialScalarGenerator() = default;

    Value generateScalar() const override;
};

class RandomizedScalarGenerator : public ScalarGenerator {
public:
    RandomizedScalarGenerator(PseudoRandom random);

    Value generateScalar() const override;

private:
    enum class ScalarChoice : int {
        kNull = 0,
        kBool = 1,
        kDouble = 2,
        kInteger = 3,
        kString = 4,
        kNumChoices = 5
    };

    std::string _generateRandomString() const;

    mutable PseudoRandom _random;
};

}  // namespace mongo
