/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/platform/decimal128.h"

#include "merizo/util/assert_util.h"

namespace merizo {

Decimal128::Decimal128(int32_t int32Value) {
    MERIZO_UNREACHABLE;
}

Decimal128::Decimal128(int64_t int64Value) {
    MERIZO_UNREACHABLE;
}

Decimal128::Decimal128(double doubleValue,
                       RoundingPrecision roundPrecision,
                       RoundingMode roundMode) {
    MERIZO_UNREACHABLE;
}

Decimal128::Decimal128(std::string stringValue, RoundingMode roundMode) {
    MERIZO_UNREACHABLE;
}

Decimal128::Value Decimal128::getValue() const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::toAbs() const {
    MERIZO_UNREACHABLE;
}

int32_t Decimal128::toInt(RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

int32_t Decimal128::toInt(uint32_t* signalingFlags, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

int64_t Decimal128::toLong(RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

int64_t Decimal128::toLong(uint32_t* signalingFlags, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

int32_t Decimal128::toIntExact(RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

int32_t Decimal128::toIntExact(uint32_t* signalingFlags, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

int64_t Decimal128::toLongExact(RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

int64_t Decimal128::toLongExact(uint32_t* signalingFlags, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

double Decimal128::toDouble(RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

double Decimal128::toDouble(uint32_t* signalingFlags, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

std::string Decimal128::toString() const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isZero() const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isNaN() const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isInfinite() const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isNegative() const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::add(const Decimal128& other, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::add(const Decimal128& other,
                           uint32_t* signalingFlags,
                           RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::subtract(const Decimal128& other, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::subtract(const Decimal128& other,
                                uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::multiply(const Decimal128& other, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::multiply(const Decimal128& other,
                                uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::divide(const Decimal128& other, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::divide(const Decimal128& other,
                              uint32_t* signalingFlags,
                              RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::quantize(const Decimal128& other, RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::quantize(const Decimal128& reference,
                                uint32_t* signalingFlags,
                                RoundingMode roundMode) const {
    MERIZO_UNREACHABLE;
}

Decimal128 Decimal128::normalize() const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isEqual(const Decimal128& other) const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isNotEqual(const Decimal128& other) const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isGreater(const Decimal128& other) const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isGreaterEqual(const Decimal128& other) const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isLess(const Decimal128& other) const {
    MERIZO_UNREACHABLE;
}

bool Decimal128::isLessEqual(const Decimal128& other) const {
    MERIZO_UNREACHABLE;
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

const Decimal128 Decimal128::kNormalizedZero = {};

}  // namespace merizo
