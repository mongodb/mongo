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

#include "mongo/db/query/optimizer/utils/unit_test_utils.h"

#include <fstream>

#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/ce/heuristic_estimator.h"
#include "mongo/db/query/ce/hinted_estimator.h"
#include "mongo/db/query/cost_model/cost_estimator_impl.h"
#include "mongo/db/query/cost_model/cost_model_manager.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/str_escape.h"


namespace mongo::optimizer {

static constexpr bool kDebugAsserts = false;

static constexpr const char* kTempFileSuffix = ".tmp.txt";

// Map from file name to a list of updates. We keep track of how many lines are added or deleted at
// a particular line of a source file.
using LineDeltaVector = std::vector<std::pair<uint64_t, int64_t>>;
std::map<std::string, LineDeltaVector> gLineDeltaMap;

// Different compilers expand __LINE__ differently. For multi-line macros, newer versions of GCC use
// the first line, while older versions use the last line. This flag accounts for the difference in
// behavior.
#if defined(__GNUC__) && (__GNUC__) >= 11
static constexpr bool kIsFirstLine = true;
#else
static constexpr bool kIsFirstLine = false;
#endif


void maybePrintABT(const ABT& abt) {
    // Always print using the supported versions to make sure we don't crash.
    const std::string strV1 = ExplainGenerator::explain(abt);
    const std::string strV2 = ExplainGenerator::explainV2(abt);
    const std::string strV2Compact = ExplainGenerator::explainV2Compact(abt);
    const std::string strBSON = ExplainGenerator::explainBSONStr(abt);

    if constexpr (kDebugAsserts) {
        std::cout << "V1: " << strV1 << "\n";
        std::cout << "V2: " << strV2 << "\n";
        std::cout << "V2Compact: " << strV2Compact << "\n";
        std::cout << "BSON: " << strBSON << "\n";
    }
}

std::string getPropsStrForExplain(const OptPhaseManager& phaseManager) {
    return ExplainGenerator::explainV2(
        make<MemoPhysicalDelegatorNode>(phaseManager.getPhysicalNodeId()),
        true /*displayPhysicalProperties*/,
        &phaseManager.getMemo());
}

static std::vector<std::string> formatStr(const std::string& str, const bool needsEscaping) {
    std::vector<std::string> replacementLines;
    std::istringstream lineInput(str);

    // Account for maximum line length after linting. We need to indent, add quotes, etc.
    static constexpr size_t kEscapedLength = 88;

    std::string line;
    while (std::getline(lineInput, line)) {
        // Read the string line by line and format it to match the test file's expected format. We
        // have an initial indentation, followed by quotes and the escaped string itself.

        std::string escaped = needsEscaping ? mongo::str::escapeForJSON(line) : line;
        for (;;) {
            // If the line is estimated to exceed the maximum length allowed by the linter, break it
            // up and make sure to insert '\n' only at the end of the last segment.
            const bool breakupLine = needsEscaping && escaped.size() > kEscapedLength;

            std::ostringstream os;
            os << "        ";
            if (needsEscaping) {
                os << "\"";
            }

            os << escaped.substr(0, kEscapedLength);

            if (needsEscaping) {
                if (!breakupLine) {
                    os << "\\n";
                }
                os << "\"";
            }
            os << "\n";

            replacementLines.push_back(os.str());

            if (breakupLine) {
                escaped = escaped.substr(kEscapedLength);
            } else {
                break;
            }
        }
    }

    if (!replacementLines.empty() && !replacementLines.back().empty()) {
        // Account for the fact that we need an extra comma after the string constant in the macro.
        auto& lastLine = replacementLines.back();
        lastLine.insert(lastLine.size() - 1, ",");

        if (replacementLines.size() == 1) {
            // For single lines, add a 'nolint' comment to prevent the linter from inlining the
            // single line with the macro itself.
            lastLine.insert(lastLine.size() - 1, "  // NOLINT (test auto-update)");
        }
    }

    return replacementLines;
}

static boost::optional<size_t> diffLookAhead(const size_t thisIndex,
                                             const std::vector<std::string>& thisSideLines,
                                             const std::string& otherLine) {
    static constexpr size_t kLookAheadSize = 5;

    const size_t maxIndex = std::min(thisIndex + kLookAheadSize, thisSideLines.size());
    for (size_t i = thisIndex + 1; i < maxIndex; i++) {
        if (thisSideLines.at(i) == otherLine) {
            return i;
        }
    }

    return {};
}

void outputDiff(std::ostream& os,
                const std::vector<std::string>& expFormatted,
                const std::vector<std::string>& actualFormatted,
                const size_t startLineNumber) {
    const size_t actualSize = actualFormatted.size();
    const size_t expSize = expFormatted.size();

    const auto outputLine =
        [&os](const bool isExpected, const size_t lineNumber, const std::string& line) {
            os << "L" << lineNumber << ": " << (isExpected ? "-" : "+") << line;
        };

    size_t expectedIndex = 0;
    size_t actualIndex = 0;
    for (;;) {
        if (actualIndex >= actualSize) {
            // Exhausted actual side.

            if (expectedIndex >= expSize) {
                // Reached the end.
                break;
            }

            // Print remaining expected side.
            const auto& expLine = expFormatted.at(expectedIndex);
            outputLine(true /*isExpected*/, startLineNumber + expectedIndex, expLine);
            expectedIndex++;
            continue;
        }

        const auto& actualLine = actualFormatted.at(actualIndex);
        if (expectedIndex >= expSize) {
            // Exhausted expected side. Print remaining actual side.
            outputLine(false /*isExpected*/, startLineNumber + actualIndex, actualLine);
            actualIndex++;
            continue;
        }

        const auto& expLine = expFormatted.at(expectedIndex);
        if (actualLine == expLine) {
            // Current lines are equal, skip.
            expectedIndex++;
            actualIndex++;
            continue;
        }

        if (const auto fwdIndex = diffLookAhead(actualIndex, actualFormatted, expLine)) {
            // Looked ahead successfully into the actual side. Move actual side forward.
            for (size_t i = actualIndex; i < *fwdIndex; i++) {
                outputLine(false /*isExpected*/, startLineNumber + i, actualFormatted.at(i));
            }
            actualIndex = *fwdIndex;
            continue;
        }
        if (const auto fwdIndex = diffLookAhead(expectedIndex, expFormatted, actualLine)) {
            // Looked ahead successfully into the expected side. Move expected side forward.
            for (size_t i = expectedIndex; i < *fwdIndex; i++) {
                outputLine(true /*isExpected*/, startLineNumber + i, expFormatted.at(i));
            }
            expectedIndex = *fwdIndex;
            continue;
        }

        // Move both sides forward.
        outputLine(false /*isExpected*/, startLineNumber + actualIndex, actualLine);
        outputLine(true /*isExpected*/, startLineNumber + expectedIndex, expLine);
        actualIndex++;
        expectedIndex++;
    }
}

bool handleAutoUpdate(const std::string& expected,
                      const std::string& actual,
                      const std::string& fileName,
                      const size_t lineNumber,
                      const bool needsEscaping) {
    if (expected == actual) {
        return true;
    }

    const auto expectedFormatted = formatStr(expected, needsEscaping);
    const auto actualFormatted = formatStr(actual, needsEscaping);

    // Treat an empty string as needing one line. Adjust for line delta.
    const size_t expectedDelta = expectedFormatted.empty() ? 1 : expectedFormatted.size();
    // Compute the macro argument start line.
    const size_t startLineNumber = kIsFirstLine ? (lineNumber + 1) : (lineNumber - expectedDelta);

    std::cout << fileName << ":" << startLineNumber << ": results differ:\n";
    outputDiff(std::cout, expectedFormatted, actualFormatted, startLineNumber);

    if (!mongo::unittest::getAutoUpdateOptimizerAsserts()) {
        std::cout << "Auto-updating is disabled.\n";
        return false;
    }

    // Compute the total number of lines added or removed before the current macro line.
    auto& lineDeltas = gLineDeltaMap.emplace(fileName, LineDeltaVector{}).first->second;
    int64_t totalDelta = 0;
    for (const auto& [line, delta] : lineDeltas) {
        if (line < startLineNumber) {
            totalDelta += delta;
        }
    }

    const size_t replacementStartLine = startLineNumber + totalDelta;
    const size_t replacementEndLine = replacementStartLine + expectedDelta;

    const std::string tempFileName = fileName + kTempFileSuffix;
    std::string line;
    size_t lineIndex = 0;

    try {
        std::ifstream in;
        in.open(fileName);
        std::ofstream out;
        out.open(tempFileName);

        // Generate a new test file, updated with the replacement string.
        while (std::getline(in, line)) {
            lineIndex++;

            if (lineIndex < replacementStartLine || lineIndex >= replacementEndLine) {
                out << line << "\n";
            } else if (lineIndex == replacementStartLine) {
                for (const auto& line1 : actualFormatted) {
                    out << line1;
                }
            }
        }

        out.close();
        in.close();

        std::rename(tempFileName.c_str(), fileName.c_str());
    } catch (const std::exception& ex) {
        // Print and re-throw exception.
        std::cout << "Caught an exception while manipulating files: " << ex.what();
        throw ex;
    }

    // Add the current delta.
    const int64_t delta = static_cast<int64_t>(actualFormatted.size()) - expectedDelta;
    lineDeltas.emplace_back(startLineNumber, delta);

    // Do not assert in order to allow multiple tests to be updated.
    return true;
}

ABT makeIndexPath(FieldPathType fieldPath, bool isMultiKey) {
    ABT result = make<PathIdentity>();

    for (size_t i = fieldPath.size(); i-- > 0;) {
        if (isMultiKey) {
            result = make<PathTraverse>(PathTraverse::kSingleLevel, std::move(result));
        }
        result = make<PathGet>(std::move(fieldPath.at(i)), std::move(result));
    }

    return result;
}

ABT makeIndexPath(FieldNameType fieldName) {
    return makeIndexPath(FieldPathType{std::move(fieldName)});
}

ABT makeNonMultikeyIndexPath(FieldNameType fieldName) {
    return makeIndexPath(FieldPathType{std::move(fieldName)}, false /*isMultiKey*/);
}

IndexDefinition makeIndexDefinition(FieldNameType fieldName, CollationOp op, bool isMultiKey) {
    IndexCollationSpec idxCollSpec{
        IndexCollationEntry((isMultiKey ? makeIndexPath(std::move(fieldName))
                                        : makeNonMultikeyIndexPath(std::move(fieldName))),
                            op)};
    return IndexDefinition{std::move(idxCollSpec), isMultiKey};
}

IndexDefinition makeCompositeIndexDefinition(std::vector<TestIndexField> indexFields,
                                             bool isMultiKey) {
    IndexCollationSpec idxCollSpec;
    for (auto& idxField : indexFields) {
        idxCollSpec.emplace_back((idxField.isMultiKey
                                      ? makeIndexPath(std::move(idxField.fieldName))
                                      : makeNonMultikeyIndexPath(std::move(idxField.fieldName))),
                                 idxField.op);
    }
    return IndexDefinition{std::move(idxCollSpec), isMultiKey};
}

std::unique_ptr<CardinalityEstimator> makeHeuristicCE() {
    return std::make_unique<ce::HeuristicEstimator>();
}

std::unique_ptr<CardinalityEstimator> makeHintedCE(ce::PartialSchemaSelHints hints) {
    return std::make_unique<ce::HintedEstimator>(std::move(hints));
}

cost_model::CostModelCoefficients getTestCostModel() {
    return cost_model::CostModelManager::getDefaultCoefficients();
}

std::unique_ptr<CostEstimator> makeCostEstimator() {
    return makeCostEstimator(getTestCostModel());
}

std::unique_ptr<CostEstimator> makeCostEstimator(
    const cost_model::CostModelCoefficients& costModel) {
    return std::make_unique<cost_model::CostEstimatorImpl>(costModel);
}


OptPhaseManager makePhaseManager(
    OptPhaseManager::PhaseSet phaseSet,
    PrefixId& prefixId,
    Metadata metadata,
    const boost::optional<cost_model::CostModelCoefficients>& costModel,
    DebugInfo debugInfo,
    QueryHints queryHints) {
    return OptPhaseManager{std::move(phaseSet),
                           prefixId,
                           false /*requireRID*/,
                           std::move(metadata),
                           makeHeuristicCE(),  // primary CE
                           makeHeuristicCE(),  // substitution phase CE, same as primary
                           makeCostEstimator(costModel ? *costModel : getTestCostModel()),
                           defaultConvertPathToInterval,
                           ConstEval::constFold,
                           std::move(debugInfo),
                           std::move(queryHints)};
}

OptPhaseManager makePhaseManager(
    OptPhaseManager::PhaseSet phaseSet,
    PrefixId& prefixId,
    Metadata metadata,
    std::unique_ptr<CardinalityEstimator> ce,
    const boost::optional<cost_model::CostModelCoefficients>& costModel,
    DebugInfo debugInfo,
    QueryHints queryHints) {
    return OptPhaseManager{std::move(phaseSet),
                           prefixId,
                           false /*requireRID*/,
                           std::move(metadata),
                           std::move(ce),      // primary CE
                           makeHeuristicCE(),  // substitution phase CE
                           makeCostEstimator(costModel ? *costModel : getTestCostModel()),
                           defaultConvertPathToInterval,
                           ConstEval::constFold,
                           std::move(debugInfo),
                           std::move(queryHints)};
}


OptPhaseManager makePhaseManagerRequireRID(OptPhaseManager::PhaseSet phaseSet,
                                           PrefixId& prefixId,
                                           Metadata metadata,
                                           DebugInfo debugInfo,
                                           QueryHints queryHints) {
    return OptPhaseManager{std::move(phaseSet),
                           prefixId,
                           true /*requireRID*/,
                           std::move(metadata),
                           makeHeuristicCE(),  // primary CE
                           makeHeuristicCE(),  // substitution phase CE, same as primary
                           makeCostEstimator(),
                           defaultConvertPathToInterval,
                           ConstEval::constFold,
                           std::move(debugInfo),
                           std::move(queryHints)};
}
}  // namespace mongo::optimizer
