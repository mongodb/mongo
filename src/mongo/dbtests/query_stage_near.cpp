/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

/**
 * This file tests near search functionality.
 */


#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/near.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

const NamespaceString kTestNamespace = NamespaceString::createNamespaceString_forTest("test.coll");
const BSONObj kTestKeyPattern = BSON("testIndex" << 1);

class QueryStageNearTest : public unittest::Test {
public:
    void setUp() override {
        _expCtx = ExpressionContextBuilder{}.opCtx(_opCtx).ns(kTestNamespace).build();

        directClient.createCollection(kTestNamespace);
        ASSERT_OK(dbtests::createIndex(_opCtx, kTestNamespace.ns_forTest(), kTestKeyPattern));

        _coll = acquireCollectionMaybeLockFree(
            _opCtx,
            CollectionAcquisitionRequest(kTestNamespace,
                                         PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kRead));
        const auto& collPtr = _coll->getCollectionPtr();
        ASSERT(collPtr);
        _mockGeoIndex = collPtr->getIndexCatalog()->findIndexByKeyPatternAndOptions(
            _opCtx, kTestKeyPattern, _makeMinimalIndexSpec(kTestKeyPattern));
        ASSERT(_mockGeoIndex);
    }

    const CollectionPtr& getCollection() const {
        return _coll->getCollectionPtr();
    }

protected:
    BSONObj _makeMinimalIndexSpec(BSONObj keyPattern) {
        return BSON(IndexDescriptor::kKeyPatternFieldName
                    << keyPattern << IndexDescriptor::kIndexVersionFieldName
                    << IndexDescriptor::getDefaultIndexVersion());
    }

    const ServiceContext::UniqueOperationContext _uniqOpCtx = cc().makeOperationContext();
    OperationContext* const _opCtx = _uniqOpCtx.get();
    DBDirectClient directClient{_opCtx};

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    boost::optional<CollectionAcquisition> _coll;
    const IndexDescriptor* _mockGeoIndex;
};

/**
 * Stage which implements a basic distance search, and interprets the "distance" field of
 * fetched documents as the distance.
 */
class MockNearStage final : public NearStage {
public:
    struct MockInterval {
        MockInterval(const std::vector<BSONObj>& data, double min, double max)
            : data(data), min(min), max(max) {}

        std::vector<BSONObj> data;
        double min;
        double max;
    };

    MockNearStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  WorkingSet* workingSet,
                  const CollectionAcquisition& coll,
                  const IndexDescriptor* indexDescriptor)
        : NearStage(expCtx.get(),
                    "MOCK_DISTANCE_SEARCH_STAGE",
                    STAGE_UNKNOWN,
                    workingSet,
                    coll,
                    indexDescriptor),
          _pos(0) {}

    void addInterval(std::vector<BSONObj> data, double min, double max) {
        _intervals.push_back(std::make_unique<MockInterval>(data, min, max));
    }

    std::unique_ptr<CoveredInterval> nextInterval(OperationContext* opCtx,
                                                  WorkingSet* workingSet) final {
        if (_pos == static_cast<int>(_intervals.size()))
            return nullptr;

        const MockInterval& interval = *_intervals[_pos++];

        bool lastInterval = _pos == static_cast<int>(_intervals.size());

        auto queuedStage = std::make_unique<QueuedDataStage>(expCtx(), workingSet);

        for (unsigned int i = 0; i < interval.data.size(); i++) {
            // Add all documents from the lastInterval into the QueuedDataStage.
            const WorkingSetID id = workingSet->allocate();
            WorkingSetMember* member = workingSet->get(id);
            member->doc = {SnapshotId(), Document{interval.data[i]}};
            member->recordId = RecordId{_pos, static_cast<int>(i)};
            workingSet->transitionToOwnedObj(id);
            queuedStage->pushBack(id);
        }

        _children.push_back(std::move(queuedStage));
        return std::make_unique<CoveredInterval>(
            _children.back().get(), interval.min, interval.max, lastInterval);
    }

    double computeDistance(WorkingSetMember* member) final {
        ASSERT(member->hasObj());
        return member->doc.value()["distance"].getDouble();
    }

    StageState initialize(OperationContext* opCtx,
                          WorkingSet* workingSet,
                          WorkingSetID* out) override {
        return IS_EOF;
    }

private:
    std::vector<std::unique_ptr<MockInterval>> _intervals;
    int _pos;
};

static std::vector<BSONObj> advanceStage(PlanStage* stage, WorkingSet* workingSet) {
    std::vector<BSONObj> results;

    WorkingSetID nextMemberID;
    PlanStage::StageState state = PlanStage::NEED_TIME;

    while (PlanStage::NEED_TIME == state) {
        while (PlanStage::ADVANCED == (state = stage->work(&nextMemberID))) {
            results.push_back(workingSet->get(nextMemberID)->doc.value().toBson());
        }
    }

    return results;
}

static void assertAscendingAndValid(const std::vector<BSONObj>& results) {
    double lastDistance = -1.0;
    for (std::vector<BSONObj>::const_iterator it = results.begin(); it != results.end(); ++it) {
        double distance = (*it)["distance"].numberDouble();
        bool shouldInclude = (*it)["$included"].eoo() || (*it)["$included"].trueValue();
        ASSERT(shouldInclude);
        ASSERT_GREATER_THAN_OR_EQUALS(distance, lastDistance);
        lastDistance = distance;
    }
}

TEST_F(QueryStageNearTest, Basic) {
    std::vector<BSONObj> mockData;
    WorkingSet workingSet;

    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);

    // First set of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 0.5));
    // Not included in this interval, but will be buffered and included in the last interval
    mockData.push_back(BSON("distance" << 2.0));
    mockData.push_back(BSON("distance" << 0.0));
    mockData.push_back(BSON("distance" << 3.5));  // Not included
    nearStage.addInterval(mockData, 0.0, 1.0);

    // Second set of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 1.5));
    mockData.push_back(BSON("distance" << 0.5));  // Not included
    mockData.push_back(BSON("distance" << 1.0));
    nearStage.addInterval(mockData, 1.0, 2.0);

    // Last set of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 2.5));
    mockData.push_back(BSON("distance" << 3.0));  // Included
    mockData.push_back(BSON("distance" << 2.0));
    mockData.push_back(BSON("distance" << 3.5));  // Not included
    nearStage.addInterval(mockData, 2.0, 3.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 8u);
    assertAscendingAndValid(results);
}

TEST_F(QueryStageNearTest, EmptyResults) {
    std::vector<BSONObj> mockData;
    WorkingSet workingSet;

    auto coll = acquireCollectionMaybeLockFree(
        _opCtx,
        CollectionAcquisitionRequest(kTestNamespace,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(_opCtx),
                                     AcquisitionPrerequisites::kRead));
    ASSERT(coll.exists());

    MockNearStage nearStage(_expCtx.get(), &workingSet, coll, _mockGeoIndex);

    // Empty set of results
    mockData.clear();
    nearStage.addInterval(mockData, 0.0, 1.0);

    // Non-empty set of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 1.5));
    mockData.push_back(BSON("distance" << 2.0));
    mockData.push_back(BSON("distance" << 1.0));
    nearStage.addInterval(mockData, 1.0, 2.0);

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 3u);
    assertAscendingAndValid(results);
}

TEST_F(QueryStageNearTest, Spilling) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagExtendedAutoSpilling", true};
    RAIIServerParameterControllerForTest maxMemoryBytes{"internalNearStageMaxMemoryBytes", 128};

    _expCtx->setTempDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp");
    _expCtx->setAllowDiskUse(true);

    WorkingSet workingSet;

    MockNearStage nearStage(_expCtx.get(), &workingSet, *_coll, _mockGeoIndex);

    static constexpr int kMaxDistance = 100;
    size_t expectedResultCount = 0;
    for (int minDistance = 0; minDistance < kMaxDistance; ++minDistance) {
        std::vector<BSONObj> mockData;
        mockData.reserve(kMaxDistance);
        for (int distance = 0; distance <= kMaxDistance; ++distance) {
            mockData.push_back(BSON("distance" << distance));
            expectedResultCount += distance >= minDistance ? 1 : 0;
        }
        nearStage.addInterval(std::move(mockData), minDistance, minDistance + 1);
    }

    std::vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), expectedResultCount);
    assertAscendingAndValid(results);

    const auto* stats = static_cast<const NearStats*>(nearStage.getSpecificStats());
    ASSERT_GT(stats->spillingStats.getSpills(), 0);
}

}  // namespace
}  // namespace mongo
