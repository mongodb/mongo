/*    Copyright 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/platform/decimal128.h"

#include "mongo/util/assert_util.h"

namespace mongo {

Decimal128::Decimal128(int32_t int32Value) {
    invariant(false);
}

Decimal128::Decimal128(int64_t int64Value) {
    invariant(false);
}

Decimal128::Decimal128(double doubleValue, RoundingMode roundMode) {
    invariant(false);
}

Decimal128::Decimal128(std::string stringValue, RoundingMode roundMode) {
    invariant(false);
}

Decimal128::Value Decimal128::getValue() const {
    invariant(false);
}

Decimal128 Decimal128::toAbs() const {
    invariant(false);
}

int32_t Decimal128::toInt(RoundingMode roundMode) const {
    invariant(false);
}

int32_t Decimal128::toInt(uint32_t* signalingFlags, RoundingMode roundMode) const {
    invariant(false);
}

int64_t Decimal128::toLong(RoundingMode roundMode) const {
    invariant(false);
}

int64_t Decimal128::toLong(uint32_t* signalingFlags, RoundingMode roundMode) const {
    invariant(false);
}

int32_t Decimal128::toIntExact(RoundingMode roundMode) const {
    invariant(false);
}

int32_t Decimal128::toIntExact(uint32_t* signalingFlags, RoundingMode roundMode) const {
    invariant(false);
}

int64_t Decimal128::toLongExact(RoundingMode roundMode) const {
    invariant(false);
}

int64_t Decimal128::toLongExact(uint32_t* signalingFlags, RoundingMode roundMode) const {
    invariant(false);
}

double Decimal128::toDouble(RoundingMode roundMode) const {
    invariant(false);
}

double Decimal128::toDouble(uint32_t* signalingFlags, RoundingMode roundMode) const {
    invariant(false);
}

std::string Decimal128::toString() const {
    invariant(false);
}

bool Decimal128::isZero() const {
    invariant(false);
}

bool Decimal128::isNaN() const {
    invariant(false);
}

bool Decimal128::isInfinite() const {
    invariant(false);
}

bool Decimal128::isNegative() const {
    invariant(false);
}

Decimal128 Decimal128::add(const Decimal128& other, RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::add(const Decimal128& other,
                           uint32_t* signalingFlags,
                           RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::subtract(const Decimal128& other, RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::subtract(const Decimal128& other,
                                uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::multiply(const Decimal128& other, RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::multiply(const Decimal128& other,
                                uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::divide(const Decimal128& other, RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::divide(const Decimal128& other,
                              uint32_t* signalingFlags,
                              RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::quantize(const Decimal128& other, RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::quantize(const Decimal128& reference,
                                uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    invariant(false);
}

Decimal128 Decimal128::normalize() const {
    invariant(false);
}

bool Decimal128::isEqual(const Decimal128& other) const {
    invariant(false);
}

bool Decimal128::isNotEqual(const Decimal128& other) const {
    invariant(false);
}

bool Decimal128::isGreater(const Decimal128& other) const {
    invariant(false);
}

bool Decimal128::isGreaterEqual(const Decimal128& other) const {
    invariant(false);
}

bool Decimal128::isLess(const Decimal128& other) const {
    invariant(false);
}

bool Decimal128::isLessEqual(const Decimal128& other) const {
    invariant(false);
}

const Decimal128 Decimal128::kLargestPositive = Decimal128();
const Decimal128 Decimal128::kSmallestPositive = Decimal128();
const Decimal128 Decimal128::kLargestNegative = Decimal128();
const Decimal128 Decimal128::kSmallestNegative = Decimal128();

const Decimal128 Decimal128::kLargestNegativeExponentZero = Decimal128();

const Decimal128 Decimal128::kPositiveInfinity = Decimal128();
const Decimal128 Decimal128::kNegativeInfinity = Decimal128();
const Decimal128 Decimal128::kPositiveNaN = Decimal128();
const Decimal128 Decimal128::kNegativeNaN = Decimal128();

}  // namespace mongo
