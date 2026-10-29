// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/query/stage_builder/sbe/abt/explain.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/platform/source_location.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>


namespace mongo::stage_builder::abt_lower {

void maybePrintABT(abt::ABT::reference_type abt);

#define ASSERT_EXPLAIN_V2(expected, abtTree)                 \
    mongo::stage_builder::abt_lower::maybePrintABT(abtTree); \
    ASSERT_EQ(expected, mongo::abt::ExplainGenerator::explainV2(abtTree))

#define ASSERT_EXPLAIN_V2_AUTO(expected, abtTree)            \
    mongo::stage_builder::abt_lower::maybePrintABT(abtTree); \
    ASSERT_STR_EQ_AUTO(expected, mongo::abt::ExplainGenerator::explainV2(abtTree))

#define ASSERT_EXPLAIN_BSON(expected, abtTree)               \
    mongo::stage_builder::abt_lower::maybePrintABT(abtTree); \
    ASSERT_EQ(expected, mongo::abt::ExplainGenerator::explainBSONStr(abtTree))

// Do not remove macro even if unused: used to update tests before committing code.
#define ASSERT_EXPLAIN_BSON_AUTO(expected, abtTree)          \
    mongo::stage_builder::abt_lower::maybePrintABT(abtTree); \
    ASSERT_STR_EQ_AUTO(expected, mongo::abt::ExplainGenerator::explainBSONStr(abtTree))

#define ASSERT_BETWEEN(a, b, value) \
    ASSERT_LTE(a, value);           \
    ASSERT_GTE(b, value);

/**
 * This is the auto-updating version of ASSERT_BETWEEN. If the value falls outside the range, we
 * create a new range which is +-25% if the value. This is expressed as a fractional operation in
 * order to preserve the type of the value (int->int, double->double).
 */
#define ASSERT_BETWEEN_AUTO(a, b, value)                                    \
    if ((value) < (a) || (value) > (b)) {                                   \
        ASSERT(AUTO_UPDATE_HELPER(str::stream() << (a) << ",\n"             \
                                                << (b),                     \
                                  str::stream() << (3 * value / 4) << ",\n" \
                                                << (5 * value / 4),         \
                                  false));                                  \
    }

}  // namespace mongo::stage_builder::abt_lower
