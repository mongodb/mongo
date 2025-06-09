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

#include "mongo/base/string_data.h"
#include "mongo/util/dynamic_bitset.h"

#include <initializer_list>
#include <iosfwd>
#include <string>
#include <vector>

namespace mongo::boolean_simplification {

/**
 * This file defines Maxterm and Minterm classes and operations over them. Maxterm/Minterms are used
 * to represent a boolean expression in a canonical form. For example, for Disjunctive Normal Form,
 * a Maxterm is used to represent the top disjunctive term and minterms are used to represent the
 * children conjunctive terms.
 */

using Bitset = DynamicBitset<size_t, 1>;

inline Bitset operator""_b(const char* bits, size_t len) {
    return Bitset{StringData{bits, len}};
}

/**
 * Represent a conjunctive or disjunctive term in a condensed bitset form.
 */
struct BitsetTerm {
    explicit BitsetTerm(size_t nbits) : predicates(nbits), mask(nbits) {}

    BitsetTerm(Bitset bitset, Bitset mask) : predicates(bitset), mask(mask) {}

    BitsetTerm(StringData bits, StringData mask)
        : BitsetTerm{Bitset{std::string{bits}}, Bitset{std::string{mask}}} {}

    BitsetTerm(size_t nbits, size_t bitIndex, bool val) : predicates(nbits), mask(nbits) {
        set(bitIndex, val);
    }

    /**
     * Flip the value of every predicate in the minterm.
     */
    void flip();

    void set(size_t bitIndex, bool value) {
        if (mask.size() <= bitIndex) {
            // This is fine from the performance perspective, because DynamicBitset will increase
            // the size by 1 block, not 1 bit.
            resize(bitIndex + 1);
        }

        mask.set(bitIndex, true);
        predicates.set(bitIndex, value);
    }

    size_t size() const {
        return mask.size();
    }

    void resize(size_t newSize) {
        predicates.resize(newSize);
        mask.resize(newSize);
    }

    /**
     * Returns true if the given terms have conflicting bits. The bits of two terms are conflicting
     * if in one term the bit is set to 1 and in another to 0.
     */
    inline bool hasConflicts(const BitsetTerm& other) const {
        return anyOf(
            [](auto predicatesBlock,
               auto maskBlock,
               auto otherPredicatesBlock,
               auto otherMaskBlock) {
                return (predicatesBlock ^ otherPredicatesBlock) & (maskBlock & otherMaskBlock);
            },
            predicates,
            mask,
            other.predicates,
            other.mask);
    }

    /**
     * Returns true if the current term can absorb the other term. For example, 'a' absorbs 'a & b'
     * (or 'a | b'). See Absorption law for details.
     */
    MONGO_COMPILER_ALWAYS_INLINE bool canAbsorb(const BitsetTerm& other) const {
        return mask.isSubsetOf(other.mask) && predicates.isEqualToMasked(other.predicates, mask);
    }

    bool isConjunctionAlwaysTrue() const {
        return mask.none();
    }

    /**
     * Predicates bitset, if a predicate takes part in the conjunction its corresponding bit in the
     * predicates bitset set to 1 if the predicate in true form or to 0 otherwise.
     */
    Bitset predicates;

    /**
     * Predicates mask, if a predicate takes part in the conjunction its corresponding bit set to 1.
     */
    Bitset mask;
};

/**
 * Minterms represent a conjunction of an expression in Disjunctive Normal Form.
 */
using Minterm = BitsetTerm;

/**
 * Maxterm represents top disjunction of an expression in Disjunctive Normal Form and consists of a
 * list of children conjunctions. Each child conjunction is represented as a Minterm.
 */
struct Maxterm {
    explicit Maxterm(size_t size);
    Maxterm(std::initializer_list<Minterm> init);

    MONGO_COMPILER_ALWAYS_INLINE Maxterm& operator|=(const Maxterm& rhs) {
        minterms.reserve(minterms.size() + rhs.minterms.size());
        for (auto& right : rhs.minterms) {
            minterms.emplace_back(right);
        }
        return *this;
    }

    MONGO_COMPILER_ALWAYS_INLINE Maxterm& operator&=(const Maxterm& rhs) {
        Maxterm result = *this & rhs;
        minterms.swap(result.minterms);
        return *this;
    }

    /**
     * Returns true if the expression is trivially true. It is recommended to call
     * removeRedundancies() before this call to make sure that always true expressions is converted
     * into trivially true one.
     */
    bool isAlwaysTrue() const;

    /**
     * Returns true if the expression is trivially false. It is recommended to call
     * removeRedundancies() before this call to make sure that always false expressions is converted
     * into trivially false one.
     */
    bool isAlwaysFalse() const;

    /**
     * Removes redundant minterms from the maxterm. A minterm might be redundant if it can be
     * absorbed by another term. For example, 'a' absorbs 'a & b'. See Absorption law for details.
     */
    void removeRedundancies();

    /**
     * Appends a new minterm with the bit at 'bitIndex' set to 'val' and all other bits unset.
     */
    void append(size_t bitIndex, bool val);

    /**
     * Appends empty minterm.
     */
    void appendEmpty();

    /**
     * Returns the number of bits that each individual minterm in the maxterm contains.
     */
    size_t numberOfBits() const {
        return _numberOfBits;
    }

    std::string toString() const;

    /**
     * Minterms represent a conjunction of an expression in Disjunctive Normal Form and consists of
     * predicates which can be in true (for a predicate A, true form is just A) of false forms (for
     * a predicate A the false form is the negation of A: ~A). Every predicate is represented by a
     * bit in the predicates bitset.
     */
    std::vector<Minterm> minterms;

private:
    size_t _numberOfBits;

    friend Maxterm operator&(const Maxterm& lhs, const Maxterm& rhs);
};

/**
 * Identify and extract common predicates from the given booleean expression in DNF. Returns the
 * pair of the extracted predicates and the expression without predicates. If there is no common
 * predicates the first element of the pair will be empty minterm.
 */
std::pair<Minterm, Maxterm> extractCommonPredicates(Maxterm maxterm);

inline Maxterm operator&(const Maxterm& lhs, const Maxterm& rhs) {
    Maxterm result{lhs._numberOfBits};
    result.minterms.reserve(lhs.minterms.size() * rhs.minterms.size());
    for (const auto& left : lhs.minterms) {
        for (const auto& right : rhs.minterms) {
            if (!left.hasConflicts(right)) {
                result.minterms.emplace_back(left.predicates | right.predicates,
                                             left.mask | right.mask);
            }
        }
    }
    return result;
}

bool operator==(const BitsetTerm& lhs, const BitsetTerm& rhs);
std::ostream& operator<<(std::ostream& os, const BitsetTerm& term);
bool operator==(const Maxterm& lhs, const Maxterm& rhs);
std::ostream& operator<<(std::ostream& os, const Maxterm& maxterm);

template <typename H>
H AbslHashValue(H h, const BitsetTerm& term) {
    return H::combine(std::move(h), term.predicates, term.mask);
}
}  // namespace mongo::boolean_simplification
