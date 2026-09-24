// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * Shared test infrastructure for the near stage unit tests. Kept in a header so that the individual
 * test files (query_stage_near.cpp and query_stage_near_residual_filter.cpp) can reuse the same
 * fixture and mock stage without duplicating code.
 */

#include "mongo/bson/bsonelement.h"
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
#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

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
        ASSERT_OK(createIndex(_opCtx, kTestNamespace.ns_forTest(), kTestKeyPattern));

        _coll = acquireCollectionMaybeLockFree(
            _opCtx,
            CollectionAcquisitionRequest(kTestNamespace,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
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
    const IndexCatalogEntry* _mockGeoIndex;
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
                  const IndexCatalogEntry* entry)
        : NearStage(
              expCtx.get(), "MOCK_DISTANCE_SEARCH_STAGE", STAGE_UNKNOWN, workingSet, coll, entry),
          _pos(0) {}

    void addInterval(std::vector<BSONObj> data, double min, double max) {
        _intervals.push_back(std::make_unique<MockInterval>(data, min, max));
    }

    /**
     * Sets the residual predicate that the NearStage applies to each document that passes the
     * distance check, mimicking a predicate pushed down from the FETCH stage above.
     */
    void setResidualFilter(std::unique_ptr<MatchExpression> filter) {
        _residualFilter = std::move(filter);
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
            // A document may specify its own RecordId via an "rid" field, which lets a test place
            // the same RecordId in more than one interval. Such a member is put in the RID_AND_OBJ
            // state, which is the state real near stages produce and the only state in which the
            // near stage deduplicates by RecordId.
            if (BSONElement rid = interval.data[i]["rid"]; !rid.eoo()) {
                member->recordId = RecordId{rid.numberLong()};
                workingSet->transitionToRecordIdAndObj(id);
            } else {
                member->recordId = RecordId{_pos, static_cast<int>(i)};
                workingSet->transitionToOwnedObj(id);
            }
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

    const MatchExpression* residualFilter() const final {
        return _residualFilter.get();
    }

    /**
     * Sets the largest distance the search can return, which the real geo near stages derive from
     * the outer bound of their search annulus.
     */
    void setMaxSearchDistance(double maxSearchDistance) {
        _maxSearchDistance = maxSearchDistance;
    }

    double maxSearchDistance() const final {
        return _maxSearchDistance;
    }

private:
    std::vector<std::unique_ptr<MockInterval>> _intervals;
    int _pos;
    std::unique_ptr<MatchExpression> _residualFilter;
    double _maxSearchDistance = std::numeric_limits<double>::infinity();
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

}  // namespace
}  // namespace mongo
