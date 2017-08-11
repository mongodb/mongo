/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/idempotency_scalar_generator.h"

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

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
    const StringData kViableChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "$.\0"_sd;
    std::size_t randomStrLength = this->_random.nextInt32(kMaxStrLength);
    std::string randomStr(randomStrLength, '\0');
    for (std::size_t i = 0; i < randomStrLength; i++) {
        randomStr[i] = kViableChars[this->_random.nextInt32(kViableChars.size())];
    }

    return randomStr;
}

}  // namespace mongo
