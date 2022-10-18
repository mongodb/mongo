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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/golden_test.h"


namespace mongo::optimizer {

ABT translatePipeline(const Metadata& metadata,
                      const std::string& pipelineStr,
                      ProjectionName scanProjName,
                      std::string scanDefName,
                      PrefixId& prefixId,
                      const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss = {});

ABT translatePipeline(Metadata& metadata,
                      const std::string& pipelineStr,
                      std::string scanDefName,
                      PrefixId& prefixId);

ABT translatePipeline(const std::string& pipelineStr, std::string scanDefName = "collection");

/**
 * This function translates the given pipeline string to an ABT and (if optimization phases are
 * provided) optimizes the ABT using the parameters specified. It then writes the output to a file
 * that will be compared to the golden testing file for the test file.
 **/
void testABTTranslationAndOptimization(
    unittest::GoldenTestContext& gctx,
    const std::string& variationName,
    const std::string& pipelineStr,
    std::string scanDefName = "collection",
    opt::unordered_set<OptPhase> phaseSet = {},
    Metadata metadata = {{{"collection", createScanDef({}, {})}}},
    PathToIntervalFn pathToInterval = {},
    bool phaseManagerDisableScan = false,
    const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss = {});

}  // namespace mongo::optimizer
