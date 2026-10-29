// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/tests/abt_unit_test_utils.h"

#include "mongo/db/query/stage_builder/sbe/abt/explain.h"

#include <cstddef>
#include <fstream>  // IWYU pragma: keep
#include <iostream>


namespace mongo::stage_builder::abt_lower {

static constexpr bool kDebugAsserts = false;

void maybePrintABT(const abt::ABT::reference_type abt) {
    // Always print using the supported versions to make sure we don't crash.
    const std::string strV2 = abt::ExplainGenerator::explainV2(abt);
    const std::string strBSON = abt::ExplainGenerator::explainBSONStr(abt);

    if constexpr (kDebugAsserts) {
        std::cout << "V2: " << strV2 << "\n";
        std::cout << "BSON: " << strBSON << "\n";
    }
}
}  // namespace mongo::stage_builder::abt_lower
