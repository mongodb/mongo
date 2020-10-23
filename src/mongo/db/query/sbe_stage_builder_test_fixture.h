/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
/**
 * SBEStageBuilderTestFixture is a unittest fixture that can be used to facilitate testing the
 * translation of a QuerySolution tree to an sbe PlanStage tree.
 *
 * The main mechanism that enables the whole sbe::PlanStage tree to be exercised under unittests is
 * the use of a VirtualScanNode. This virtual node can be created with a vector of BSONArray
 * documents and used as a replacement for a CollectionScanNode in the QuerySolution tree. A testing
 * client would manually build a QuerySolution tree containing this VirtualScanNode and
 * then transform it to an sbe::PlanStage by calling buildPlanStage(). The buildPlanStage()
 * method will do the QuerySolution to sbe::PlanStage translation, and return a vector of result
 * slots, the prepared sub-tree and an sbe::PlanStageData that carries the sbe::CompileCtx needed to
 * prepare the sbe::PlanStage tree. The sbe::PlanStageData returned from buildPlanStage() must be
 * kept alive across buildPlanStage(), prepareTree() and execution of the plan.
 */
class SBEStageBuilderTestFixture : public sbe::PlanStageTestFixture {
public:
    SBEStageBuilderTestFixture() = default;

    /**
     * Makes a QuerySolution from a QuerySolutionNode.
     */
    std::unique_ptr<QuerySolution> makeQuerySolution(std::unique_ptr<QuerySolutionNode> root);

    /**
     * Builds an sbe::PlanStage tree from a QuerySolution that can be executed by repeatedly calling
     * getNext() on the root. Results from the PlanStage are exposed through the returned
     * SlotVector. If hasRecordId is 'true' then the returned SlotVector will carry a SlotId in the
     * 0th position for the RecordId and a SlotId for the BSONObj representation of the document in
     * the 1st position. Otherwise, if hasRecordId is 'false', the SlotVector will contain a single
     * SlotId for the BSONObj representation of the document.
     */
    std::
        tuple<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>
        buildPlanStage(std::unique_ptr<QuerySolution> querySolution, bool hasRecordId);

private:
    const NamespaceString _nss = NamespaceString{"testdb.sbe_stage_builder"};
};

}  // namespace mongo
