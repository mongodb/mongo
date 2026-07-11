// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <vector>

#include <absl/container/inlined_vector.h>

namespace mongo::boolean_simplification {
/**
 * A bitset whose set bits correspond to those original minterms covered by a derived minterm
 * (a.k.a. prime implicant).
 */
using CoveredOriginalMinterms = DynamicBitset<size_t, 2>;

/**
 * Represents a list of prime implicants, identified by their indices.
 */
using PrimeImplicantIndices = std::vector<uint32_t>;


/**
 * An implementation of Petrick's Method: https://en.wikipedia.org/wiki/Petrick%27s_method. This is
 * an algorithm for finding a minimum sum-of-products expression given as input a list of prime
 * implicants found by the Quine-McCluskey algorithm. The outer list has an element for each minterm
 * found by the Quine-McCluskey algorithm. The inner list describes which of the original minterms
 * are covered by that prime implicant minterm, referring to the indexes of the original minterms.
 * Returns a list of a minimal sets of the indices of the original minterms that covers all of the
 * original minterms. The caller is expected to choose a coverage which has the fewest number of
 * minterms, and if there is still a tie to choose the coverage with the fewest number of literals.
 * For example, consider the following input [[1, 2, 3], [3, 4], [0, 4, 5]]. We can see that the
 * original maxterm given to the Quine-McCluskey algorithm has 6 minterms, indexed from 0 to 5. The
 * Quine-McCluskey algorithm managed to simplify the original maxterm to the new one with just 3
 * minterms, indexed from 0 to 2. For this example, we expect the output to be [[0, 2]], because
 * only 2 prime implicants with indices 0 and 2 are enough to cover all 6 original minterms. It is
 * possible that we can get more than one coverage as output. For the given input: [[0, 1, 2], [2,
 * 3], [0, 3]] two coverages are possible: [[0, 1], [0, 2]].
 *
 * 'maxNumPrimeImplicants' specifies a threshold for the maximum number of prime implicants we may
 * produce, in order to bound both the time complexity and memory footprint of this function. If we
 * hit this limit, this returns an empty vector.
 */
std::vector<PrimeImplicantIndices> petricksMethod(const std::vector<CoveredOriginalMinterms>& data,
                                                  size_t maxNumPrimeImplicants);
}  // namespace mongo::boolean_simplification
