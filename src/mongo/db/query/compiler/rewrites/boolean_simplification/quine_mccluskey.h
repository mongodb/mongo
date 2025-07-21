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

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/petrick.h"

#include <utility>
#include <vector>

namespace mongo::boolean_simplification {
/**
 * This is an implementation of the finding prime implicants step of the Quine–McCluskey algorithm.
 * Quine-McCluskey is a method used for minimizing boolean expressions. The function takes a maxterm
 * to simplify and returns the simplified maxterm (aka prime implicants) and a vector of indices of
 * covered input minterms by the corresponding derived minterms. The vector of covered input
 * minterms is supposed to be used in the next optimization step that calculates a minimal coverage.
 * See 'petricksMethod()' for details. For example, consider the following input ~A~B~C~D | ~A~B~CD
 * | ~AB~C~D | ~AB~CD | ~ABCD | A~BCD, which can be represented as the maxterm: [(0001, 1111),
 * (0100, 1111), (0101, 1111), (0111, 1111), (1011, 1111)], where the first bitset in every pair
 * represents predicates and the second one prepresents mask (see Maxtern and Minterm documentation
 * for details). The prime implicants for the expression will be A~BCD, ~ABD, ~A~C or represented as
 * minterms: [(1011, 1111), (0101, 1101), (0000, 1010)] and the input minterms coverage will be:
 * [5], [3, 4], [0, 1, 2, 3], which means that the first prime implicant A~BCD covers only the input
 * minterms with index 5, which has the same value A~BCD. And the second prime implicant ~ABD covers
 * input minterms with indexes 3 and 4 which are ~AB~CD, ~ABCD.
 * Indeed ~AB~CD | ~ABCD == ~ABD & (~C | C) == ~ABD.
 */
std::pair<Maxterm, std::vector<CoveredOriginalMinterms>> findPrimeImplicants(Maxterm maxterm);

/**
 * The Quine–McCluskey algorithm is a method used for minimizing boolean expressions. It works by
 * comparing pairs of minterms and merging them into derived minterms. This process continues until
 * no further simplification is possible. The function takes a maxterm to simplify and returns the
 * simplified maxterm. For example, consider the following input ~A~B~C~D | ~A~B~CD | ~AB~C~D |
 * ~AB~CD |~ABCD | A~BCD, which can be represented as the maxterm: [(0001, 1111), (0100, 1111),
 * (0101, 1111), (0111, 1111), (1011, 1111)], where the first bitset in every pair represents
 * predicates and the second one prepresents mask (see Maxtern and Minterm documentation for
 * details). This expression can be simplified to A~BCD | ~ABD | ~A~C, or [(1011, 1111), (0101,
 * 1101), (0000, 1010)] in maxterm/minterm representation.
 */
Maxterm quineMcCluskey(Maxterm maxterm);

}  // namespace mongo::boolean_simplification
