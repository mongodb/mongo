/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <limits>
#include <vector>

#include "mongo/db/query/optimizer/defs.h"

namespace mongo::ce {

using namespace mongo::optimizer;

// Default cardinality when actual collection cardinality is unknown.
// Mostly used by unit tests.
constexpr CEType kDefaultCard = 1000.00;

// Minimum estimated cardinality. In the absense of any statistics we can never
// assume there are less than this many matching documents.
constexpr CEType kMinCard = 0.01;

/**
 * Specifies the maximum number of elements (selectivities) to use when estimating via
 * exponential backoff.
 */
const size_t kMaxBackoffElements = 4;


bool validSelectivity(SelectivityType sel);

bool validCardinality(CEType card);

/**
 * Estimates the selectivity of a conjunction given the selectivities of its subexpressions using
 * exponential backoff.
 */
SelectivityType conjExponentialBackoff(std::vector<SelectivityType> conjSelectivities);

/**
 * Estimates the selectivity of a disjunction given the selectivities of its subexpressions using
 * exponential backoff.
 */
SelectivityType disjExponentialBackoff(std::vector<SelectivityType> disjSelectivities);
}  // namespace mongo::ce
