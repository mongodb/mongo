// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/boolean_simplification/quine_mccluskey.h"


// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/petrick.h"
#include "mongo/stdx/unordered_set.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include <absl/container/btree_map.h>

namespace mongo::boolean_simplification {
namespace {

struct MintermData {
    MintermData(CoveredOriginalMinterms coveredMinterms)
        : coveredMinterms(std::move(coveredMinterms)), combined(false) {}

    // Bitset where each bit corresponds to the index of one original input minterm. A bit is set
    // when its corresponding minterm is "covered" by the current derived minterm. Note that the
    // original minterm is covered by all minterms which are produced by combinations of the
    // original minterm.
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
            // The current minterm must cover itself.
            CoveredOriginalMinterms coveredMinterms{minterms.size()};
            coveredMinterms.set(i);
            insert(std::move(minterms[i]), std::move(coveredMinterms));
        }
    }

    void insert(Minterm minterm, CoveredOriginalMinterms coveredMinterms) {
        const auto count = minterm.predicates.count();
        if (table.size() <= count) {
            table.resize(count + 1);
        }
        table[count].emplace(std::move(minterm), std::move(coveredMinterms));
    }

    bool empty() const {
        return table.empty();
    }

    size_t size() const {
        return table.size();
    }

    // Table of minterms where each row has minterms with the same number of true predicates.
    // Note: we need these to be ordered & stable, which is why we don't use an absl::hash_map.
    std::vector<absl::btree_map<Minterm, MintermData>> table;
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
        for (auto& [lMinterm, lhs] : qmc.table[i]) {
            for (auto& [rMinterm, rhs] : qmc.table[i + 1]) {
                // We combine two minterms if and only if:
                // 1. They have the same mask.
                if (lMinterm.mask != rMinterm.mask) {
                    continue;
                }
                const auto differentBits = lMinterm.predicates ^ rMinterm.predicates;
                // 2. The number of true predicates differs by 1.
                if (differentBits.count() == 1) {
                    lhs.combined = true;
                    rhs.combined = true;

                    // Main QMC step: Adding the new combined minterm which is a combination of two
                    // minterms which have the same masks and the number of set bits in the
                    // predicates differs by 1. Now we can use this minterm only instead of the two
                    // originals. It unsets the differing bit from the mask.
                    result.insert(Minterm{lMinterm.predicates & rMinterm.predicates,
                                          lMinterm.mask & ~differentBits},
                                  lhs.coveredMinterms | rhs.coveredMinterms);
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
            for (auto&& [minterm, data] : mintermDataRow) {
                // If the minterm was not combined during this step we need to preserve it in the
                // result.
                if (!data.combined && seenMinterms.insert(minterm).second) {
                    result.first.minterms.emplace_back(std::move(minterm));
                    result.second.emplace_back(std::move(data.coveredMinterms));
                }
            }
        }

        std::swap(qmc, combinedTable);
    }

    return result;
}

Maxterm quineMcCluskey(Maxterm inputMaxterm, size_t maxNumPrimeImplicants) {
    auto [maxterm, maxtermCoverage] = findPrimeImplicants(std::move(inputMaxterm));
    const bool allEssential = std::all_of(maxtermCoverage.begin(),
                                          maxtermCoverage.end(),
                                          [](const auto& cov) { return cov.size() == 1; });

    if (allEssential) {
        return maxterm;
    }

    const auto& primeImplicantCoverages = petricksMethod(maxtermCoverage, maxNumPrimeImplicants);
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
