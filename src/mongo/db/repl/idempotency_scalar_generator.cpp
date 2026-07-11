// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/idempotency_scalar_generator.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/assert_util.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

Value TrivialScalarGenerator::generateScalar() const {
    return Value(0);
}

RandomizedScalarGenerator::RandomizedScalarGenerator(PseudoRandom random) : _random(random) {}

Value RandomizedScalarGenerator::generateScalar() const {
    auto randomInt = this->_random.nextInt32(static_cast<int32_t>(ScalarChoice::kNumChoices));
    auto choice = static_cast<ScalarChoice>(randomInt);
    auto randomBit = this->_random.nextInt32(2);
    auto multiplier = randomBit == 1 ? 1 : -1;
    switch (choice) {
        case ScalarChoice::kNull:
            return Value(BSONNULL);
        case ScalarChoice::kBool:
            return Value(randomBit == 1);
        case ScalarChoice::kDouble:
            return Value(this->_random.nextCanonicalDouble() * INT_MAX * multiplier);
        case ScalarChoice::kInteger:
            return Value(this->_random.nextInt32(INT_MAX) * multiplier);
        case ScalarChoice::kString:
            return Value(_generateRandomString());
        case ScalarChoice::kNumChoices:
            MONGO_UNREACHABLE
    }
    MONGO_UNREACHABLE;
}

std::string RandomizedScalarGenerator::_generateRandomString() const {
    const std::size_t kMaxStrLength = 500;
    const std::string_view kViableChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "$.\0"sv;
    std::size_t randomStrLength = this->_random.nextInt32(kMaxStrLength);
    std::string randomStr(randomStrLength, '\0');
    for (std::size_t i = 0; i < randomStrLength; i++) {
        randomStr[i] = kViableChars[this->_random.nextInt32(kViableChars.size())];
    }

    return randomStr;
}

}  // namespace mongo
