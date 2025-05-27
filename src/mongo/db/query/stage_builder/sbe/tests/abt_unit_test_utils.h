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

#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/query/stage_builder/sbe/abt/explain.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/platform/source_location.h"
#include "mongo/unittest/unittest.h"

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
