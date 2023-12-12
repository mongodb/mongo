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

#include "mongo/db/query/boolean_simplification/bitset_algebra.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <ostream>
#include <utility>

#include "mongo/util/assert_util.h"
#include "mongo/util/stream_utils.h"

namespace mongo::boolean_simplification {
void BitsetTerm::flip() {
    predicates.flip();
    predicates &= mask;
}

Maxterm::Maxterm(size_t size) : _numberOfBits(size) {}

Maxterm::Maxterm(std::initializer_list<Minterm> init)
    : minterms(std::move(init)), _numberOfBits(0) {
    tassert(7507918, "Maxterm cannot be initilized with empty list of minterms", !minterms.empty());
    for (auto& minterm : minterms) {
        _numberOfBits = std::max(minterm.size(), _numberOfBits);
    }

    for (auto& minterm : minterms) {
        if (_numberOfBits > minterm.size()) {
            minterm.resize(_numberOfBits);
        }
    }
}

bool Maxterm::isAlwaysTrue() const {
    return minterms.size() == 1 && minterms.front().isConjunctionAlwaysTrue();
}

bool Maxterm::isAlwaysFalse() const {
    return minterms.empty();
}

std::string Maxterm::toString() const {
    std::ostringstream oss{};
    oss << *this;
    return oss.str();
}

void Maxterm::removeRedundancies() {
    std::sort(minterms.begin(), minterms.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.mask.count() < rhs.mask.count();
    });

    std::vector<Minterm> newMinterms{};
    newMinterms.reserve(minterms.size());

    for (auto&& minterm : minterms) {
        bool absorbed = false;
        for (const auto& seenMinterm : newMinterms) {
            if (seenMinterm.canAbsorb(minterm)) {
                absorbed = true;
                break;
            }
        }
        if (!absorbed) {
            newMinterms.emplace_back(std::move(minterm));
        }
    }

    minterms.swap(newMinterms);
}

void Maxterm::append(size_t bitIndex, bool val) {
    minterms.emplace_back(_numberOfBits, bitIndex, val);
}

void Maxterm::appendEmpty() {
    minterms.emplace_back(_numberOfBits);
}

std::pair<Minterm, Maxterm> extractCommonPredicates(Maxterm maxterm) {
    if (maxterm.minterms.empty()) {
        return {Minterm{maxterm.numberOfBits()}, std::move(maxterm)};
    }

    Bitset commonTruePredicates{maxterm.numberOfBits()};
    commonTruePredicates.set();

    Bitset commonFalsePredicates{maxterm.numberOfBits()};
    commonFalsePredicates.set();

    for (const auto& minterm : maxterm.minterms) {
        commonTruePredicates &= minterm.predicates;
        commonFalsePredicates &= (minterm.mask ^ minterm.predicates);
    }

    bool isMaxtermAlwaysTrue = false;

    // Remove common true predicates from the maxterm.
    if (commonTruePredicates.any()) {
        for (auto& minterm : maxterm.minterms) {
            auto setCommon = minterm.predicates & commonTruePredicates;
            minterm.predicates &= ~setCommon;
            minterm.mask &= ~setCommon;
            isMaxtermAlwaysTrue = isMaxtermAlwaysTrue | minterm.mask.none();
        }
    }

    // Remove common false predicates from the maxterm.
    if (commonFalsePredicates.any()) {
        for (auto& minterm : maxterm.minterms) {
            auto setCommon = (minterm.mask ^ minterm.predicates) & commonFalsePredicates;
            minterm.mask &= ~setCommon;
            isMaxtermAlwaysTrue = isMaxtermAlwaysTrue | minterm.mask.none();
        }
    }

    if (isMaxtermAlwaysTrue) {
        maxterm.minterms.clear();
        maxterm.appendEmpty();
    }

    Minterm commonPredicates{commonTruePredicates, commonTruePredicates | commonFalsePredicates};
    return {std::move(commonPredicates), std::move(maxterm)};
}

bool operator==(const BitsetTerm& lhs, const BitsetTerm& rhs) {
    return lhs.predicates == rhs.predicates && lhs.mask == rhs.mask;
}

std::ostream& operator<<(std::ostream& os, const BitsetTerm& term) {
    os << '(' << term.predicates << ", " << term.mask << ")";
    return os;
}

bool operator==(const Maxterm& lhs, const Maxterm& rhs) {
    return lhs.minterms == rhs.minterms;
}

std::ostream& operator<<(std::ostream& os, const Maxterm& maxterm) {
    using mongo::operator<<;
    return os << maxterm.minterms;
}
}  // namespace mongo::boolean_simplification
