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

#include "mongo/db/query/boolean_simplification/petrick.h"

#include <boost/dynamic_bitset.hpp>

#include "mongo/util/assert_util.h"

namespace mongo::boolean_simplification {
namespace {

class PrimeImplicant {
public:
    PrimeImplicant() {}

    PrimeImplicant(size_t numberOfBits, size_t implicantIndex) : _implicant(numberOfBits) {
        _implicant.set(implicantIndex);
    }

    explicit PrimeImplicant(boost::dynamic_bitset<size_t> bitset) : _implicant(std::move(bitset)) {}

    /**
     * Returns true if 'this' is a non-strict subset of 'other'.
     */
    bool isSubset(const PrimeImplicant& other) const {
        return (_implicant & other._implicant) == _implicant;
    }

    std::vector<uint32_t> getListOfSetBits() const {
        std::vector<uint32_t> result{};
        for (uint32_t i = _implicant.find_first(); i < _implicant.size();
             i = _implicant.find_next(i)) {
            result.emplace_back(i);
        }
        return result;
    }

    size_t numberOfSetBits() const {
        return _implicant.count();
    }

    friend PrimeImplicant operator|(const PrimeImplicant& lhs, const PrimeImplicant& rhs);

private:
    boost::dynamic_bitset<size_t> _implicant;
};

PrimeImplicant operator|(const PrimeImplicant& lhs, const PrimeImplicant& rhs) {
    return PrimeImplicant(lhs._implicant | rhs._implicant);
}

/**
 * Sum (union) of prime implicants.
 */
class ImplicantSum {
public:
    void appendNewImplicant(size_t numberOfBits, size_t implicantIndex) {
        _implicants.emplace_back(numberOfBits, implicantIndex);
    }

    /**
     * Inserts the 'implicant'. Uses the absorption law to minimize the number of
     * implicants. Three outcomes are possible: 1. The implicant is inserted. 2. The implicant is
     * inserted and some existing implicants are removed due to being absorbed by the new one. 3.
     * The implicant is not inserted because it was absorbed by one of the existing implicants.
     */
    void insert(PrimeImplicant implicant) {
        size_t size = _implicants.size();
        size_t pos = 0;
        const int32_t implicantNumberOfSetBits = static_cast<int32_t>(implicant.numberOfSetBits());

        while (pos < size) {
            auto& current = _implicants[pos];
            const int32_t diffInNumberOfSetBits =
                static_cast<int32_t>(current.numberOfSetBits()) - implicantNumberOfSetBits;
            // Here we apply the absorption law: X + XY = X.
            if (diffInNumberOfSetBits <= 0 && current.isSubset(implicant)) {
                // Current is a non-strict subset of the new implicant, we don't need to add
                // implicant.
                return;
            } else if (diffInNumberOfSetBits > 0 && implicant.isSubset(current)) {
                // New implicant is a subset of the current, it means we remove the current, by
                // swapping the current element with the last element. The last elements will be
                // deleted in the end of the function by calling resize().
                --size;
                std::swap(current, _implicants[size]);
                --pos;
            }
            ++pos;
        }

        // Erase removed elements and allocate memory for the new one if required.
        _implicants.resize(size + 1);
        // Insert new implicant.
        _implicants[_implicants.size() - 1] = std::move(implicant);
    }

    /**
     * Finds the product of two implicant sums using De Morgan's laws.
     */
    ImplicantSum product(const ImplicantSum& other) const {
        // E.g., one implicant sum covers minterms with indices 0 and 1, and another with 0 and 2.
        // (I0 + I1) * (I0 + I2) = I0 + I0*I2 + I0*I1 + I1*I2.
        ImplicantSum result{};
        for (const auto& l : _implicants) {
            for (const auto& r : other._implicants) {
                auto implicant = l | r;
                // Trying to add to the result every applicant we got here. In the example above it
                // would be I0, I0*I2, I0*I1, I1*I2. 'insertImplicant()' applies the absorption
                // law (X + XY = X) to minimize number of implicants. In the example only I0
                // and I1*I2 would be added, because I0 "absorbs" I0*I2 and I0*I1.
                result.insert(std::move(implicant));
            }
        }

        return result;
    }

    void swap(ImplicantSum& other) {
        _implicants.swap(other._implicants);
    }

    /**
     * Expands a bitset representation of each prime implicant into a vector of minterm indexes and
     * returns the resulting vector.
     */
    std::vector<std::vector<uint32_t>> getCoverages() const {
        std::vector<std::vector<uint32_t>> result{};
        result.reserve(_implicants.size());

        for (const auto& implicant : _implicants) {
            result.emplace_back(implicant.getListOfSetBits());
        }

        return result;
    }

private:
    std::vector<PrimeImplicant> _implicants;
};

/**
 * The Petrick's method implementation using tabular approach.
 */
class TabularPetrick {
public:
    explicit TabularPetrick(const std::vector<std::vector<unsigned>>& data)
        : _numberOfBits(data.size()) {
        for (size_t implicantIndex = 0; implicantIndex < data.size(); ++implicantIndex) {
            for (auto mintermIndex : data[implicantIndex]) {
                insert(mintermIndex, implicantIndex);
            }
        }
    }

    std::vector<std::vector<uint32_t>> getMinimalCoverages() {
        while (_table.size() > 1) {
            const size_t size = _table.size();
            auto productResult = _table[size - 1].product(_table[size - 2]);
            _table.pop_back();
            _table[_table.size() - 1].swap(productResult);
        }

        return _table.front().getCoverages();
    }

private:
    void insert(size_t mintermIndex, size_t implicantIndex) {
        if (_table.size() <= mintermIndex) {
            _table.resize(mintermIndex + 1);
        }

        _table[mintermIndex].appendNewImplicant(_numberOfBits, implicantIndex);
    };

    const size_t _numberOfBits;
    std::vector<ImplicantSum> _table;
};
}  // namespace

std::vector<std::vector<uint32_t>> petricksMethod(const std::vector<std::vector<uint32_t>>& data) {
    if (data.empty()) {
        return {};
    }
    TabularPetrick table{data};
    return table.getMinimalCoverages();
}
}  // namespace mongo::boolean_simplification
