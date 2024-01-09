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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/golden_test_base.h"

namespace mongo::optimizer {

ABT translatePipeline(const Metadata& metadata,
                      StringData pipelineStr,
                      ProjectionName scanProjName,
                      std::string scanDefName,
                      PrefixId& prefixId,
                      const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss = {},
                      bool shouldParameterize = false,
                      QueryParameterMap* = nullptr,
                      size_t maxDepth = kMaxPathConjunctionDecomposition);

void formatGoldenTestHeader(StringData variationName,
                            StringData pipelineStr,
                            StringData findCmd,
                            std::string scanDefName,
                            opt::unordered_set<OptPhase> phaseSet,
                            Metadata metadata,
                            std::ostream& stream);

std::string formatGoldenTestExplain(ABT translated, std::ostream& stream);

/**
 * Fixture for ABT tests which use the golden testing infrastructure.
 */
class ABTGoldenTestFixture : public ServiceContextTest {
public:
    static constexpr StringData kConfigPath = "src/mongo/db/test_output/pipeline/abt"_sd;

    ABTGoldenTestFixture()
        : _config{kConfigPath.toString()},
          _ctx(std::make_unique<unittest::GoldenTestContext>(&_config)) {}

    void tearDown() override {
        ServiceContextTest::tearDown();
        // Deleted early so it won't throw in the destructor.
        // Throwing from the destructor would violate the base
        // class destructor's noexcept spec.
        // It's not allowed by `std::unique_ptr`, either.
        delete _ctx.release();
    }

protected:
    /**
     * This function translates the given pipeline string to an ABT and (if optimization phases are
     * provided) optimizes the ABT using the parameters specified. It then writes the output to a
     * file that will be compared to the golden testing file for the test file. The function returns
     * the explained optimized plan.
     **/
    std::string testABTTranslationAndOptimization(
        StringData variationName,
        StringData pipelineStr,
        std::string scanDefName = "collection",
        opt::unordered_set<OptPhase> phaseSet = {},
        Metadata metadata = {{{"collection", createScanDef({}, {})}}},
        PathToIntervalFn pathToInterval = {},
        bool phaseManagerDisableScan = false,
        const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss = {});

    std::string testParameterizedABTTranslation(StringData variationName,
                                                StringData findCmd = "",
                                                StringData pipelineStr = "",
                                                std::string scanDefName = "collection",
                                                Metadata metadata = {
                                                    {{"collection", createScanDef({}, {})}}});

private:
    unittest::GoldenTestConfig _config;
    std::unique_ptr<unittest::GoldenTestContext> _ctx;
};

}  // namespace mongo::optimizer
