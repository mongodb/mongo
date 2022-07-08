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

#include "mongo/db/query/ce/ce_test_utils.h"

#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

using namespace optimizer;
using namespace cascades;

CETester::CETester(std::string collName, double numRecords)
    : _collName(std::move(collName)), _numRecords(std::move(numRecords)) {}

double CETester::getCE(const std::string& query) {
    // Mock opCtx for test.
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    // Mock memo.
    ScanDefinition sd({}, {}, {DistributionType::Centralized}, true, _numRecords);
    Metadata metadata({{_collName, sd}});
    Memo memo(DebugInfo::kDefaultForTests,
              metadata,
              std::make_unique<DefaultLogicalPropsDerivation>(),
              std::make_unique<HeuristicCE>());

    // Construct placeholder PhaseManager.
    PrefixId prefixId;
    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{_collName, {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);

    // Construct ABT from pipeline and optimize.
    ABT abt = translatePipeline("[{$match: " + query + "}]", _collName);
    ASSERT_TRUE(phaseManager.optimize(abt));

    // Get cardinality estimate.
    auto cht = getCETransport(opCtx.get());
    return cht->deriveCE(memo, {}, abt.ref());
}

}  // namespace mongo::ce
