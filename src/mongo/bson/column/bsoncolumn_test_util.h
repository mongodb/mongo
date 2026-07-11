// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/exec/sbe/values/bsoncolumn_materializer.h"
#include "mongo/util/modules.h"

namespace mongo::bsoncolumn {

/**
 * Returns true if the binaries of the SBE values are equal. This function assumes the tags have
 * already been validated.
 */
bool areSBEBinariesEqual(sbe::bsoncolumn::SBEColumnMaterializer::Element& actual,
                         sbe::bsoncolumn::SBEColumnMaterializer::Element& expected);

/**
 * The expected min and max of a BSONColumn, each paired with the logical index of the first
 * occurrence. If first.eoo(), the column had no defined values and second is undefined.
 */
struct ExpectedMinMax {
    std::pair<BSONElement, size_t> min;
    std::pair<BSONElement, size_t> max;
};

/**
 * Compute the expected min and max from a vector of elements.
 */
ExpectedMinMax expectedMinMax(std::vector<BSONElement>& elems);

}  // namespace mongo::bsoncolumn
