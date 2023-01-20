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

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/query/ce/hinted_estimator.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/utils.h"


namespace mongo::optimizer {

void maybePrintABT(const ABT& abt);

std::string getPropsStrForExplain(const OptPhaseManager& phaseManager);

/**
 * Computes a difference between the expected and actual formatted output and outputs it to the
 * provide stream instance. Used to display difference between expected and actual format for
 * auto-update macros. It is exposed in the header here for testability.
 */
void outputDiff(std::ostream& os,
                const std::vector<std::string>& expFormatted,
                const std::vector<std::string>& actualFormatted,
                size_t startLineNumber);

bool handleAutoUpdate(const std::string& expected,
                      const std::string& actual,
                      const std::string& fileName,
                      size_t lineNumber,
                      bool needsEscaping);

/**
 * Auto update result back in the source file if the assert fails.
 * The expected result must be a multi-line string in the following form:
 *
 * ASSERT_EXPLAIN_V2_AUTO(     // NOLINT
 *       "BinaryOp [Add]\n"
 *       "|   Const [2]\n"
 *       "Const [1]\n",
 *       tree);
 *
 * Limitations:
 *      1. There should not be any comments or other formatting inside the multi-line string
 *      constant other than 'NOLINT'. If we have a single-line constant, the auto-updating will
 *      generate a 'NOLINT' at the end of the line.
 *      2. The expression which we are explaining ('tree' in the example above) must fit on a single
 *      line.
 *      3. The macro should be indented by 4 spaces.
 */
#define AUTO_UPDATE_HELPER(expected, actual, needsEscaping) \
    handleAutoUpdate(expected, actual, __FILE__, __LINE__, needsEscaping)

#define ASSERT_STR_EQ_AUTO(expected, actual) ASSERT(AUTO_UPDATE_HELPER(expected, actual, true))


#define ASSERT_EXPLAIN(expected, abt) \
    maybePrintABT(abt);               \
    ASSERT_EQ(expected, ExplainGenerator::explain(abt))

#define ASSERT_EXPLAIN_AUTO(expected, abt) \
    maybePrintABT(abt);                    \
    ASSERT_STR_EQ_AUTO(expected, ExplainGenerator::explain(abt))


#define ASSERT_EXPLAIN_V2(expected, abt) \
    maybePrintABT(abt);                  \
    ASSERT_EQ(expected, ExplainGenerator::explainV2(abt))

#define ASSERT_EXPLAIN_V2_AUTO(expected, abt) \
    maybePrintABT(abt);                       \
    ASSERT_STR_EQ_AUTO(expected, ExplainGenerator::explainV2(abt))


#define ASSERT_EXPLAIN_V2Compact(expected, abt) \
    maybePrintABT(abt);                         \
    ASSERT_EQ(expected, ExplainGenerator::explainV2Compact(abt))

#define ASSERT_EXPLAIN_V2Compact_AUTO(expected, abt) \
    maybePrintABT(abt);                              \
    ASSERT_STR_EQ_AUTO(expected, ExplainGenerator::explainV2Compact(abt))


#define ASSERT_EXPLAIN_BSON(expected, abt) \
    maybePrintABT(abt);                    \
    ASSERT_EQ(expected, ExplainGenerator::explainBSONStr(abt))

// Do not remove macro even if unused: used to update tests before committing code.
#define ASSERT_EXPLAIN_BSON_AUTO(expected, abt) \
    maybePrintABT(abt);                         \
    ASSERT_STR_EQ_AUTO(expected, ExplainGenerator::explainBSONStr(abt))


#define ASSERT_EXPLAIN_PROPS_V2(expected, phaseManager) \
    ASSERT_EQ(expected, getPropsStrForExplain(phaseManager))

#define ASSERT_EXPLAIN_PROPS_V2_AUTO(expected, phaseManager) \
    ASSERT_STR_EQ_AUTO(expected, getPropsStrForExplain(phaseManager))


#define ASSERT_EXPLAIN_MEMO(expected, memo) ASSERT_EQ(expected, ExplainGenerator::explainMemo(memo))

#define ASSERT_EXPLAIN_MEMO_AUTO(expected, memo) \
    ASSERT_STR_EQ_AUTO(expected, ExplainGenerator::explainMemo(memo))


#define ASSERT_INTERVAL(expected, interval) \
    ASSERT_EQ(expected, ExplainGenerator::explainIntervalExpr(interval))

#define ASSERT_INTERVAL_AUTO(expected, interval) \
    ASSERT_STR_EQ_AUTO(expected, ExplainGenerator::explainIntervalExpr(interval))


#define ASSERT_BSON_PATH(expected, bson, path)                      \
    ASSERT_EQ(expected,                                             \
              dotted_path_support::extractElementAtPath(bson, path) \
                  .toString(false /*includeFieldName*/));


#define ASSERT_NUMBER_EQ_AUTO(expected, actual) \
    ASSERT(AUTO_UPDATE_HELPER(str::stream() << expected, str::stream() << actual, false))

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

struct TestIndexField {
    FieldNameType fieldName;
    CollationOp op;
    bool isMultiKey;
};

ABT makeIndexPath(FieldPathType fieldPath, bool isMultiKey = true);

ABT makeIndexPath(FieldNameType fieldName);
ABT makeNonMultikeyIndexPath(FieldNameType fieldName);

/**
 * Constructs metadata for an an index on a single, non-dotted field.
 */
IndexDefinition makeIndexDefinition(FieldNameType fieldName,
                                    CollationOp op,
                                    bool isMultiKey = true);
IndexDefinition makeCompositeIndexDefinition(std::vector<TestIndexField> indexFields,
                                             bool isMultiKey = true);

/**
 * A factory function to create a heuristic-based cardinality estimator.
 */
std::unique_ptr<CardinalityEstimator> makeHeuristicCE();

/**
 * A factory function to create a hint-based cardinality estimator.
 */
std::unique_ptr<CardinalityEstimator> makeHintedCE(ce::PartialSchemaSelHints hints);

/**
 * Return default CostModel used in unit tests.
 */
cost_model::CostModelCoefficients getTestCostModel();

/*
 * A convenience factory function to create costing with default CostModel.
 */
std::unique_ptr<CostEstimator> makeCostEstimator();

/**
 * A convenience factory function to create costing with overriden CostModel.
 */
std::unique_ptr<CostEstimator> makeCostEstimator(
    const cost_model::CostModelCoefficients& costModel);

/**
 * A convenience factory function to create OptPhaseManager for unit tests with cost model.
 */
OptPhaseManager makePhaseManager(
    OptPhaseManager::PhaseSet phaseSet,
    PrefixId& prefixId,
    Metadata metadata,
    const boost::optional<cost_model::CostModelCoefficients>& costModel,
    DebugInfo debugInfo,
    QueryHints queryHints = {});

/**
 * A convenience factory function to create OptPhaseManager for unit tests with CE hints and cost
 * model.
 */
OptPhaseManager makePhaseManager(
    OptPhaseManager::PhaseSet phaseSet,
    PrefixId& prefixId,
    Metadata metadata,
    std::unique_ptr<CardinalityEstimator> ce,
    const boost::optional<cost_model::CostModelCoefficients>& costModel,
    DebugInfo debugInfo,
    QueryHints queryHints = {});

/**
 * A convenience factory function to create OptPhaseManager for unit tests which requires RID.
 */
OptPhaseManager makePhaseManagerRequireRID(OptPhaseManager::PhaseSet phaseSet,
                                           PrefixId& prefixId,
                                           Metadata metadata,
                                           DebugInfo debugInfo,
                                           QueryHints queryHints = {});

}  // namespace mongo::optimizer
