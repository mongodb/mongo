// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains minimal tests for sbe::ScanStage.
 */

#include "mongo/bson/json.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/exchange_gather.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe {

namespace {

/**
 * Consumes its whole input during open() and counts the documents it produces; the result is
 * returned as a single NumberInt64 in 'outSlot' by the first call to getNext(). By default every
 * row produced by the input counts as one document. If 'sumSlot' is set, the input rows carry
 * partial counts in that slot (e.g. the output of other MockCountStages running below an
 * ExchangeGather), and the partial counts are summed up instead.
 */
class MockCountStage final : public PlanStage {
public:
    MockCountStage(std::unique_ptr<PlanStage> child,
                   value::SlotId outSlot,
                   boost::optional<value::SlotId> sumSlot = boost::none)
        : PlanStage("mock_count"sv, nullptr, kEmptyPlanNodeId, false),
          _outSlot(outSlot),
          _sumSlot(sumSlot) {
        _children.emplace_back(std::move(child));
    }

    std::unique_ptr<PlanStage> clone() const final {
        return std::make_unique<MockCountStage>(_children[0]->clone(), _outSlot, _sumSlot);
    }

    /**
     * Supports parallel execution iff the child does: wraps each clone of the child in its own
     * MockCountStage, so that each clone consumes its share of the input during open().
     */
    std::vector<std::unique_ptr<PlanStage>> cloneForParallelExecution(CompileCtx& ctx,
                                                                      size_t dop) const final {
        auto clones = _children[0]->cloneForParallelExecution(ctx, dop);
        for (auto& c : clones) {
            c = std::make_unique<MockCountStage>(std::move(c), _outSlot, _sumSlot);
        }
        return clones;
    }

    void prepare(CompileCtx& ctx) final {
        _children[0]->prepare(ctx);
        if (_sumSlot) {
            _sumAccessor = _children[0]->getAccessor(ctx, *_sumSlot);
        }
    }

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final {
        if (slot == _outSlot) {
            return &_outAccessor;
        }
        return _children[0]->getAccessor(ctx, slot);
    }

    void open(bool reOpen) final {
        _commonStats.opens++;
        _children[0]->open(reOpen);
        _count = 0;
        while (_children[0]->getNext() == PlanState::ADVANCED) {
            if (_sumAccessor) {
                auto [tag, val] = _sumAccessor->getViewOfValue();
                ASSERT_EQ(tag, value::TypeTags::NumberInt64);
                _count += value::bitcastTo<int64_t>(val);
            } else {
                ++_count;
            }
        }
        _returned = false;
        _outAccessor.reset();
    }

    PlanState getNext() final {
        if (_returned) {
            _outAccessor.reset();
            return trackPlanState(PlanState::IS_EOF);
        }
        _returned = true;
        _outAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(_count));
        return trackPlanState(PlanState::ADVANCED);
    }

    void close() final {
        trackClose();
        _children[0]->close();
        _outAccessor.reset();
    }

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final {
        auto stats = std::make_unique<PlanStageStats>(_commonStats);
        stats->children.emplace_back(_children[0]->getStats(includeDebugInfo));
        return stats;
    }

    const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    void doDebugPrint(std::vector<DebugPrinter::Block>&, DebugPrintInfo&) const final {}

    size_t estimateCompileTimeSize() const final {
        return sizeof(*this) + _children[0]->estimateCompileTimeSize();
    }

private:
    const value::SlotId _outSlot;
    const boost::optional<value::SlotId> _sumSlot;

    value::SlotAccessor* _sumAccessor{nullptr};
    value::ViewOfValueAccessor _outAccessor;
    int64_t _count{0};
    bool _returned{false};
};

}  // namespace

class ScanStageTest : public PlanStageTestFixture {
public:
    void insertDocuments(const std::vector<BSONObj>& docs) {
        AutoGetCollection agc(operationContext(), _nss, LockMode::MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(operationContext(), *agc, docs));
        wuow.commit();

        // Capture the RecordIds assigned to the documents, in insertion order.
        auto cursor = agc->getCursor(operationContext());
        while (auto record = cursor->next()) {
            _recordIds.push_back(record->id);
        }
    }

    MultipleCollectionAccessor createCollection(const std::vector<BSONObj>& docs,
                                                boost::optional<BSONObj> indexKeyPattern) {
        // Create collection and index
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), _nss, CollectionOptions()));
        if (indexKeyPattern) {
            ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
                operationContext(),
                _nss,
                {BSON("v" << 2 << "name" << DBClientBase::genIndexName(*indexKeyPattern) << "key"
                          << *indexKeyPattern)}));
        }
        insertDocuments(docs);

        auto localColl =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), _nss, AcquisitionPrerequisites::kRead),
                              MODE_IS);
        MultipleCollectionAccessor colls(localColl);
        return colls;
    }

    enum class ScanKind { kGeneric, kScan };

    static std::unique_ptr<PlanStage> makeScan(
        ScanKind kind,
        const UUID& uuid,
        const DatabaseName& dbName,
        value::SlotId scanSlot,
        std::vector<std::string> fieldNames,
        value::SlotVector fieldSlots,
        bool forward,
        boost::optional<value::SlotId> recordIdSlot = boost::none,
        boost::optional<value::SlotId> minRecordIdSlot = boost::none,
        boost::optional<value::SlotId> maxRecordIdSlot = boost::none) {
        if (kind == ScanKind::kGeneric) {
            // GenericScanStage does not support record id ranges.
            tassert(13436601,
                    "GenericScanStage has no record id range",
                    !recordIdSlot && !minRecordIdSlot && !maxRecordIdSlot);
            return sbe::makeS<sbe::GenericScanStage>(uuid,
                                                     dbName,
                                                     scanSlot,
                                                     /*recordIdSlot=*/boost::none,
                                                     /*snapshotIdSlot=*/boost::none,
                                                     /*indexIdentSlot=*/boost::none,
                                                     /*indexKeySlot=*/boost::none,
                                                     /*indexKeyPatternSlot=*/boost::none,
                                                     std::move(fieldNames),
                                                     std::move(fieldSlots),
                                                     forward,
                                                     /*yieldPolicy=*/nullptr,
                                                     kEmptyPlanNodeId,
                                                     /*scanOpenCallback=*/nullptr,
                                                     /*participateInTrialRunTracking=*/false);
        }
        return sbe::makeS<sbe::ScanStage>(uuid,
                                          dbName,
                                          scanSlot,
                                          recordIdSlot,
                                          /*snapshotIdSlot=*/boost::none,
                                          /*indexIdentSlot=*/boost::none,
                                          /*indexKeySlot=*/boost::none,
                                          /*indexKeyPatternSlot=*/boost::none,
                                          std::move(fieldNames),
                                          std::move(fieldSlots),
                                          minRecordIdSlot,
                                          maxRecordIdSlot,
                                          forward,
                                          /*yieldPolicy=*/nullptr,
                                          kEmptyPlanNodeId,
                                          /*scanOpenCallback=*/nullptr,
                                          /*participateInTrialRunTracking=*/false);
    }

    void runScan(ScanKind kind, bool forward);
    void runScanParallel(ScanKind kind, bool forward);
    void runParallelReopen(ScanKind kind);

protected:
    // RecordIds of the inserted documents, in insertion order.
    std::vector<RecordId> _recordIds;
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("testdb.sbe_scan_stage");
};

void ScanStageTest::runScan(ScanKind kind, bool forward) {
    auto colls =
        createCollection({fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}")}, boost::none);
    UUID uuid = colls.getMainCollection()->uuid();
    DatabaseName dbName = _nss.dbName();

    value::TagValueOwned input =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSONArray()));
    value::TagValueOwned expected = value::TagValueOwned::fromRaw(stage_builder::makeValue(
        forward ? BSON_ARRAY(BSON("_id" << 0 << "a" << 1) << BSON("_id" << 1 << "a" << 2))
                : BSON_ARRAY(BSON("_id" << 1 << "a" << 2) << BSON("_id" << 0 << "a" << 1))));

    auto makeStageFn = [kind, uuid, dbName, forward](value::SlotId scanSlot,
                                                     std::unique_ptr<PlanStage> stage) {
        auto scanStage = makeScan(kind,
                                  uuid,
                                  dbName,
                                  scanSlot,
                                  /*fieldNames=*/{},
                                  /*fieldSlots=*/{},
                                  forward);
        return std::make_pair(scanSlot, std::move(scanStage));
    };

    attachCollectionAcquisition(colls);
    auto [inputTag, inputVal] = input.releaseToRaw();
    auto [expectedTag, expectedVal] = expected.releaseToRaw();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ScanStageTest, genericScanStage) {
    runScan(ScanKind::kGeneric, /*forward=*/true);
}

TEST_F(ScanStageTest, genericScanStageBackward) {
    runScan(ScanKind::kGeneric, /*forward=*/false);
}

TEST_F(ScanStageTest, scanStage) {
    runScan(ScanKind::kScan, /*forward=*/true);
}

TEST_F(ScanStageTest, scanStageBackward) {
    runScan(ScanKind::kScan, /*forward=*/false);
}

void ScanStageTest::runScanParallel(ScanKind kind, bool forward) {
    // Enough documents for the random cursor to sample the boundaries of many more chunks than
    // there are threads.
    constexpr int kNumDocs = 500;
    std::vector<BSONObj> docs;
    std::vector<int> expected;
    for (int i = 0; i < kNumDocs; ++i) {
        docs.push_back(BSON("_id" << i << "a" << i));
        expected.push_back(i);
    }
    auto colls = createCollection(docs, boost::none);
    UUID uuid = colls.getMainCollection()->uuid();
    DatabaseName dbName = _nss.dbName();

    // The ScanStage can be restricted to a range of RecordIds, passed in through the
    // RuntimeEnvironment. Both bounds are inclusive.
    auto env = std::make_unique<RuntimeEnvironment>();
    boost::optional<value::SlotId> minSlot, maxSlot;
    if (kind == ScanKind::kScan) {
        constexpr int kFirst = 100, kLast = 400;
        ASSERT_EQ(_recordIds.size(), kNumDocs);
        auto registerRid = [&](const RecordId& rid) {
            auto [tag, val] = value::makeCopyRecordId(rid);
            auto slot = generateSlotId();
            env->registerSlot(tag, val, true, slot);
            return slot;
        };
        minSlot = registerRid(_recordIds[kFirst]);
        maxSlot = registerRid(_recordIds[kLast]);
        expected.assign(expected.begin() + kFirst, expected.begin() + kLast + 1);
    }

    auto makeStageFn = [this, kind, uuid, dbName, forward, minSlot, maxSlot](
                           value::SlotId scanSlot, std::unique_ptr<PlanStage> stage) {
        auto fieldSlot = generateSlotId();
        auto scanStage = makeScan(kind,
                                  uuid,
                                  dbName,
                                  scanSlot,
                                  /*fieldNames=*/{"a"},
                                  /*fieldSlots=*/{fieldSlot},
                                  forward,
                                  // The end bound is only enforced when the RecordId is exposed.
                                  minSlot ? boost::make_optional(generateSlotId()) : boost::none,
                                  minSlot,
                                  maxSlot);
        auto gather = sbe::makeS<sbe::ExchangeGather>(std::move(scanStage),
                                                      sbe::value::SlotVector{scanSlot, fieldSlot},
                                                      /*dop=*/4,
                                                      /*yieldPolicy=*/nullptr,
                                                      kEmptyPlanNodeId,
                                                      /*participateInTrialRunTracking=*/false);
        return std::make_pair(fieldSlot, std::move(gather));
    };

    attachCollectionAcquisition(colls);
    auto [fieldSlot, root] = makeStageFn(generateSlotId(), nullptr);
    auto ctx = makeCompileCtx(std::move(env));
    auto* accessor = prepareTree(ctx.get(), root.get(), fieldSlot);

    // Drain the plan directly rather than through getAllResults(), which yields between calls:
    // ExchangeGather does not support yielding over a collection scan yet.
    std::vector<int> results;
    while (root->getNext() == PlanState::ADVANCED) {
        auto [tag, val] = accessor->getViewOfValue();
        ASSERT_EQ(tag, value::TypeTags::NumberInt32);
        results.push_back(value::bitcastTo<int32_t>(val));
    }
    root->close();

    // Every document must be returned exactly once, in no particular order.
    std::sort(results.begin(), results.end());
    ASSERT_EQ(results, expected);
}

TEST_F(ScanStageTest, genericScanStageParallel) {
    runScanParallel(ScanKind::kGeneric, /*forward=*/true);
}

TEST_F(ScanStageTest, genericScanStageParallelBackward) {
    runScanParallel(ScanKind::kGeneric, /*forward=*/false);
}

TEST_F(ScanStageTest, scanStageParallel) {
    runScanParallel(ScanKind::kScan, /*forward=*/true);
}

TEST_F(ScanStageTest, scanStageParallelBackward) {
    runScanParallel(ScanKind::kScan, /*forward=*/false);
}

void ScanStageTest::runParallelReopen(ScanKind kind) {
    constexpr int kNumDocs = 500;
    std::vector<BSONObj> docs;
    for (int i = 0; i < kNumDocs; ++i) {
        docs.push_back(BSON("_id" << i << "a" << i));
    }
    auto colls = createCollection(docs, boost::none);
    UUID uuid = colls.getMainCollection()->uuid();
    DatabaseName dbName = _nss.dbName();

    // MockCount -> ExchangeGather -> MockCount -> Scan. The lower MockCount consumes its share of
    // the scan in open(), which exercises the reset of the shared chunks performed by each clone;
    // the upper one sums up the partial counts, so the result must be the size of the collection.
    auto scanSlot = generateSlotId();
    auto partialCountSlot = generateSlotId();
    auto totalCountSlot = generateSlotId();
    auto scan = makeScan(kind,
                         uuid,
                         dbName,
                         scanSlot,
                         /*fieldNames=*/{},
                         /*fieldSlots=*/{},
                         /*forward=*/true);
    auto partialCount = std::make_unique<MockCountStage>(std::move(scan), partialCountSlot);
    auto gather = sbe::makeS<sbe::ExchangeGather>(std::move(partialCount),
                                                  sbe::value::SlotVector{partialCountSlot},
                                                  /*dop=*/4,
                                                  /*yieldPolicy=*/nullptr,
                                                  kEmptyPlanNodeId,
                                                  /*participateInTrialRunTracking=*/false);
    auto root =
        std::make_unique<MockCountStage>(std::move(gather), totalCountSlot, partialCountSlot);

    attachCollectionAcquisition(colls);
    auto ctx = makeCompileCtx();
    // Prepares the tree and calls open(false) a first time.
    auto* accessor = prepareTree(ctx.get(), root.get(), totalCountSlot);

    // ExchangeGather must have cloned the lower MockCount, or it would have run single threaded.
    auto rootStats = root->getStats(/*includeDebugInfo=*/true);
    ASSERT_EQ(rootStats->children.size(), 1u);
    const auto& gatherStats = *rootStats->children[0];
    ASSERT_GTE(gatherStats.debugInfo["actualDop"].numberLong(), 2);

    auto checkResult = [&] {
        ASSERT_EQ(root->getNext(), PlanState::ADVANCED);
        auto [tag, val] = accessor->getViewOfValue();
        ASSERT_EQ(tag, value::TypeTags::NumberInt64);
        ASSERT_EQ(value::bitcastTo<int64_t>(val), kNumDocs);
        ASSERT_EQ(root->getNext(), PlanState::IS_EOF);
    };

    // Run the open()/getNext()/close() sequence twice on the same plan.
    checkResult();
    root->close();
    root->open(false);
    checkResult();
    root->close();

    // Skip the close(): open()/getNext()/open(true)/getNext().
    root->open(false);
    checkResult();
    root->open(true);
    checkResult();
    root->close();
}

TEST_F(ScanStageTest, genericScanStageParallelReopen) {
    runParallelReopen(ScanKind::kGeneric);
}

TEST_F(ScanStageTest, scanStageParallelReopen) {
    runParallelReopen(ScanKind::kScan);
}

}  // namespace mongo::sbe
