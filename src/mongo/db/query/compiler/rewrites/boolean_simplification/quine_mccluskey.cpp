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

#include "mongo/db/query/compiler/rewrites/boolean_simplification/quine_mccluskey.h"

#include <absl/container/node_hash_set.h>
#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/stdx/unordered_set.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>

namespace mongo::boolean_simplification {
namespace {

struct MintermData {
    MintermData(Minterm minterm, CoveredOriginalMinterms coveredMinterms)
        : minterm(std::move(minterm)),
          coveredMinterms(std::move(coveredMinterms)),
          combined(false) {}

    Minterm minterm;

    // List of indices of original input minterms which are "covered" by the current derived
    // minterm. The original minterm is covered by all minterms which are produced
    // by combinations of the original minterm.
    CoveredOriginalMinterms coveredMinterms;

    // Set to true for minterms which are combination of at least two other minterms.
    bool combined;
};

/**
 * A utility class that helps to organise minterms by the number of bits set. This is the main
 * internal data structure of the Quine–McCluskey algorithm. It contains minterms organized by
 * number of bits set to 1 in predicates list. The QMC algorithm can combine minterms which have the
 * same mask and the number of bits in the predicates differ by 1.
 */
struct QmcTable {
    explicit QmcTable(size_t maximumSize) {
        table.reserve(maximumSize);
    }

    QmcTable(std::vector<Minterm> minterms) {
        size_t size = 0;
        for (const auto& minterm : minterms) {
            size = std::max(minterm.predicates.count(), size);
        }
        table.resize(size);

        for (uint32_t i = 0; i < static_cast<uint32_t>(minterms.size()); ++i) {
            insert(std::move(minterms[i]), CoveredOriginalMinterms{i});
        }
    }

    void insert(Minterm minterm, CoveredOriginalMinterms coveredMinterms) {
        const auto count = minterm.predicates.count();
        if (table.size() <= count) {
            table.resize(count + 1);
        }
        table[count].emplace_back(std::move(minterm), std::move(coveredMinterms));
    }

    bool empty() const {
        return table.empty();
    }

    size_t size() const {
        return table.size();
    }

    // List of minterms origanized by number of true predicates.
    std::vector<std::vector<MintermData>> table;
};

/**
 * Main step of the Quine-McCluskey method. It combines minterms that differ by only one bit and
 * builds a new QMC table to be used for the next iteration.
 */
QmcTable combine(QmcTable& qmc) {
    QmcTable result{qmc.size()};

    for (size_t i = 0; i < qmc.table.size() - 1; ++i) {
        // QmcTable organizes minterms by number of true predicates in them. Therefore, here we
        // always try to combine minterms where the number of true predicates differ by 1.
        for (auto& lhs : qmc.table[i]) {
            for (auto& rhs : qmc.table[i + 1]) {
                // We combine two minterms if and only if:
                // 1. They have the same mask.
                if (lhs.minterm.mask != rhs.minterm.mask) {
                    continue;
                }
                const auto differentBits = lhs.minterm.predicates ^ rhs.minterm.predicates;
                // 2. The number of true predicates differs by 1.
                if (differentBits.count() == 1) {
                    lhs.combined = true;
                    rhs.combined = true;

                    CoveredOriginalMinterms coveredMinterms{};
                    coveredMinterms.reserve(lhs.coveredMinterms.size() +
                                            rhs.coveredMinterms.size());
                    std::merge(begin(lhs.coveredMinterms),
                               end(lhs.coveredMinterms),
                               begin(rhs.coveredMinterms),
                               end(rhs.coveredMinterms),
                               back_inserter(coveredMinterms));
                    // Main QMC step: Adding the new combined minterm which is a combination of two
                    // minterms which have the same masks and the number of set bits in the
                    // predicates differs by 1. Now we can use this minterm only instead of the two
                    // originals. It unsets the differing bit from the mask.
                    result.insert(Minterm{lhs.minterm.predicates & rhs.minterm.predicates,
                                          lhs.minterm.mask & ~differentBits},
                                  std::move(coveredMinterms));
                }
            }
        }
    }
    return result;
}

size_t getCoverageCost(const PrimeImplicantIndices& coverage, const Maxterm& maxterm) {
    size_t cost = coverage.size() * maxterm.numberOfBits();
    for (const auto& mintermIndex : coverage) {
        cost += maxterm.minterms[mintermIndex].mask.count();
    }
    return cost;
}

/**
 * Choose a coverage which has the fewest number of minterms, and if there is still a tie to
 * choose the coverage with the fewest number of literals.
 */
const PrimeImplicantIndices& findOptimalCoverage(
    const std::vector<PrimeImplicantIndices>& coverages, const Maxterm& maxterm) {
    return *std::min_element(
        begin(coverages), end(coverages), [&maxterm](const auto& lhs, const auto& rhs) {
            return getCoverageCost(lhs, maxterm) < getCoverageCost(rhs, maxterm);
        });
}
}  // namespace

std::pair<Maxterm, std::vector<CoveredOriginalMinterms>> findPrimeImplicants(Maxterm maxterm) {
    std::pair<Maxterm, std::vector<CoveredOriginalMinterms>> result{Maxterm{maxterm.numberOfBits()},
                                                                    {}};
    QmcTable qmc{std::move(maxterm.minterms)};
    stdx::unordered_set<Minterm> seenMinterms{};

    while (!qmc.empty()) {
        auto combinedTable = combine(qmc);

        for (auto&& mintermDataRow : qmc.table) {
            for (auto&& mintermData : mintermDataRow) {
                Minterm minterm{std::move(mintermData.minterm)};
                // If the minterm was not combined during this step we need to preserve it in the
                // result.
                if (!mintermData.combined && seenMinterms.insert(minterm).second) {
                    result.first.minterms.emplace_back(std::move(minterm));
                    result.second.emplace_back(std::move(mintermData.coveredMinterms));
                }
            }
        }

        std::swap(qmc, combinedTable);
    }

    return result;
}

Maxterm quineMcCluskey(Maxterm inputMaxterm) {
    auto [maxterm, maxtermCoverage] = findPrimeImplicants(std::move(inputMaxterm));
    const bool allEssential = std::all_of(maxtermCoverage.begin(),
                                          maxtermCoverage.end(),
                                          [](const auto& cov) { return cov.size() == 1; });
    if (allEssential) {
        return maxterm;
    }
    const auto& primeImplicantCoverages = petricksMethod(maxtermCoverage);
    if (primeImplicantCoverages.size() < 2) {
        return maxterm;
    }

    const auto& minCoverage = findOptimalCoverage(primeImplicantCoverages, maxterm);

    // All minterms are included into the minumal coverage.
    if (minCoverage.size() == maxterm.minterms.size()) {
        return maxterm;
    }

    std::vector<Minterm> selectedMinterms{};
    selectedMinterms.reserve(minCoverage.size());
    for (const auto& mintermIndex : minCoverage) {
        selectedMinterms.emplace_back(std::move(maxterm.minterms[mintermIndex]));
    }

    maxterm.minterms.swap(selectedMinterms);
    return maxterm;
}

}  // namespace mongo::boolean_simplification
