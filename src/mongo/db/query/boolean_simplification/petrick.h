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

#include <cstdint>
#include <vector>

#include <absl/container/inlined_vector.h>

namespace mongo::boolean_simplification {
/**
 * The list of original minterms covered by a derived minterm (a.k.a. prime implicant).
 */
using CoveredOriginalMinterms = absl::InlinedVector<uint32_t, 2>;

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
 */
std::vector<PrimeImplicantIndices> petricksMethod(const std::vector<CoveredOriginalMinterms>& data);
}  // namespace mongo::boolean_simplification
