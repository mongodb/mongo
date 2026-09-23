// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/express/plan_executor_express.h"

#include "mongo/bson/json.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
IndexEntry createIndexEntry(BSONObj keyPattern, bool unique, bool sparse = false) {
    return IndexEntry(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      IndexConfig::kLatestIndexVersion,
                      false /*multikey*/,
                      {} /*mutikeyPaths*/,
                      {} /*multikeyPathSet*/,
                      sparse,
                      unique,
                      IndexEntry::Identifier{"ident"},
                      BSONObj() /*infoObj*/,
                      nullptr /*wildcardProjection*/);
}

class ExpressTest : public ServiceContextTest {
public:
    std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                                 bool setLimit1 = false,
                                                 const char* projectionStr = nullptr) {
        auto nss = NamespaceString::createNamespaceString_forTest("test.collection");
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(fromjson(queryStr));
        if (setLimit1) {
            findCommand->setLimit(1);
        }
        if (projectionStr) {
            findCommand->setProjection(fromjson(projectionStr));
        }
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    OperationContext* opCtx() {
        if (!_opCtxOwned) {
            _opCtxOwned = makeOperationContext();
        }

        return _opCtxOwned.get();
    }

    ServiceContext::UniqueOperationContext _opCtxOwned;
};

/**
 * Drives a real 'PlanExecutorExpress' over a real collection, which the eligibility tests above
 * cannot do. This is what pins termination end to end: 'PlanExecutorExpress::getNext()' loops while
 * it has no output and breaks only on 'ExpressPlan::exhausted()', so an iterator that reports a
 * non-terminal result on a fetch miss spins in the executor, not in the iterator.
 */
class ExpressExecutorTest : public CatalogTestFixture {
protected:
    static inline const NamespaceString kNss =
        NamespaceString::createNamespaceString_forTest("ExpressExecutorTest.TestCollection");
    static constexpr auto kIndexName = "a_1";

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));

        auto collection = acquire();
        {
            WriteUnitOfWork wuow(operationContext());
            CollectionWriter writer{operationContext(), &collection};
            auto* writable = writer.getWritableCollection(operationContext());
            ASSERT_OK(writable->getIndexCatalog()->createIndexOnEmptyCollection(
                operationContext(),
                writable,
                BSON("v" << 2 << "name" << kIndexName << "key" << BSON("a" << 1))));
            wuow.commit();
        }
        {
            WriteUnitOfWork wuow(operationContext());
            std::vector<BSONObj> docs{fromjson("{_id: 0, a: 2}"), fromjson("{_id: 1, a: 5}")};
            ASSERT_OK(Helpers::insert(operationContext(), collection.getCollectionPtr(), docs));
            wuow.commit();
        }
    }

    CollectionAcquisition acquire() {
        return acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(kNss,
                                         PlacementConcern(boost::none, boost::none),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kRead),
            MODE_X);
    }

    /**
     * Deletes the record behind the 'a: 5' index key without touching the index, leaving an entry
     * that references a record which no longer exists.
     */
    void orphanIndexKeyForValueFive(const CollectionPtr& collectionPtr) {
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        auto* recordStore = collectionPtr->getRecordStore();

        boost::optional<RecordId> orphanedRid;
        {
            auto cursor = recordStore->getCursor(operationContext(), ru);
            while (auto record = cursor->next()) {
                if (record->data.toBson().getIntField("a") == 5) {
                    orphanedRid = record->id;
                    break;
                }
            }
        }
        ASSERT(bool(orphanedRid));

        WriteUnitOfWork wuow(operationContext());
        recordStore->deleteRecord(operationContext(), ru, *orphanedRid);
        wuow.commit();
    }

    std::unique_ptr<CanonicalQuery> canonicalizeFindByA() {
        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(fromjson("{a: 5}"));
        findCommand->setLimit(1);
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx =
                ExpressionContextBuilder{}.fromRequest(operationContext(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    IndexForExpressEquality indexForA(const CollectionPtr& collectionPtr,
                                      const CanonicalQuery& cq) {
        boost::optional<IndexEntry> entry;
        for (auto&& ice : collectionPtr->getIndexCatalog()->getEntriesShared(
                 IndexCatalog::InclusionPolicy::kReady)) {
            if (ice->descriptor()->indexName() == kIndexName) {
                entry = indexEntryFromIndexCatalogEntry(operationContext(), collectionPtr, ice, cq);
            }
        }
        ASSERT(bool(entry));
        return IndexForExpressEquality(std::move(*entry), false /* coversProjection */);
    }
};

// A fetch miss must drain to EOF within a short deadline, having examined the orphaned key exactly
// once. The two assertions are complementary: the key count rules out an executor that loops a
// bounded number of times, and the deadline rules out one that never terminates at all.
TEST_F(ExpressExecutorTest, orphanIndexKeyReturnsEofOnPlainRead) {
    auto collection = acquire();
    orphanIndexKeyForValueFive(collection.getCollectionPtr());

    // Matches what the command layer gives an ordinary find, which is the configuration that spun.
    shard_role_details::getRecoveryUnit(operationContext())
        ->setPrepareConflictBehavior(PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    auto cq = canonicalizeFindByA();
    auto index = indexForA(collection.getCollectionPtr(), *cq);
    auto exec = makeExpressExecutorForFindByUserIndex(operationContext(),
                                                      std::move(cq),
                                                      collection,
                                                      index,
                                                      boost::none /* collectionFilter */,
                                                      false /* returnOwnedBson */);

    // Bounds only the drain loop, so setup cost cannot eat into it. One seek and one failed fetch
    // is microseconds of work; seconds of headroom covers a loaded sanitizer host.
    operationContext()->setDeadlineByDate(Date_t::now() + Seconds(5), ErrorCodes::MaxTimeMSExpired);

    BSONObj out;
    ASSERT_EQ(exec->getNext(&out, nullptr), PlanExecutor::IS_EOF);

    // EOF alone would also be reported by an executor that looped a hundred times before giving
    // up. Pinning the key count is what proves the orphaned entry was seeked once.
    PlanSummaryStats summaryStats;
    exec->getPlanExplainer().getSummaryStats(&summaryStats);
    ASSERT_EQ(summaryStats.totalKeysExamined, 1);
    ASSERT_EQ(summaryStats.totalDocsExamined, 0);
    ASSERT_EQ(summaryStats.nReturned, 0);
}

}  // namespace

TEST_F(ExpressTest, idIndexEligibility) {
    auto cq = canonicalize("{_id: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{createIndexEntry(BSON("_id" << 1), true), false};
    params.mainCollectionInfo.indexes = {createIndexEntry(BSON("_id" << 1), false),
                                         createIndexEntry(BSON("a" << 1), true),
                                         createIndexEntry(BSON("_id" << 1 << "f"
                                                                     << "1"),
                                                          true),
                                         want.index};
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    // Shard filtering shouldn't change anything since _id indexes are always unique.
    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);
}

TEST_F(ExpressTest, compoundIdIndexEligibility) {
    auto cq = canonicalize("{_id: 2}", /* limit 1*/ true);
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{createIndexEntry(BSON("_id" << 1 << "f"
                                                             << "1"),
                                                  true),
                                 false};
    params.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1), true),
                                         createIndexEntry(BSON("_id" << 1 << "f"
                                                                     << "1"),
                                                          false),
                                         want.index};
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    // Unique compound indexes ineligible when shard filtering is required.
    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);

    // Unique compound indexes + no shard filtering still ineligible if no limit(1).
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto cqNoLimit1 = canonicalize("{_id: 2}", /* limit 1*/ false);
    ent = getIndexForExpressEquality(*cqNoLimit1, params);
    ASSERT_EQUALS(ent, boost::none);
}

// Non-_id query prefers unique, single-field index.
TEST_F(ExpressTest, singleFieldIndexEligibility) {
    auto cq = canonicalize("{a: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{createIndexEntry(BSON("a" << 1), true), false};
    params.mainCollectionInfo.indexes = {createIndexEntry(BSON("_id" << 1), true),
                                         createIndexEntry(BSON("a" << 1), false),
                                         createIndexEntry(BSON("a" << 1 << "f"
                                                                   << "1"),
                                                          true),
                                         want.index};
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);
}

// Non-unique single-field or compound indexes are ineligible without limit(1) and no
// shard filtering. The query must be guaranteed to produce at most 1 result.
TEST_F(ExpressTest, nonUniqueIndexEligibility) {
    auto cq = canonicalize("{a: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("a" << 1), false),
        createIndexEntry(BSON("a" << 1 << "f"
                                  << "1"),
                         false),
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);

    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);

    // Non-unique index also ineligible with .limit(1) if shard filtering is needed
    cq = canonicalize("{a: 2}", true /* limit 1 */);
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);

    // Non-unique index is eligible with limit(1) and no shard filtering
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent->index, params.mainCollectionInfo.indexes[1]);
}

TEST_F(ExpressTest, compoundUniqueIndexEligibility) {
    auto cq = canonicalize("{a: 2}", true /* limit 1 */);
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{createIndexEntry(BSON("a" << 1 << "f"
                                                           << "1"),
                                                  true),
                                 false};
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("a" << 1), false),
        want.index,
        createIndexEntry(BSON("a" << 1 << "f"
                                  << "1"),
                         false),
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    // Compound unique indexes are ineligible when shard filtering is required.
    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);

    // Compound unique indexes always ineligible when there's no .limit(1).
    cq = canonicalize("{a: 2}", false /* limit 1 */);
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);

    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);
}

TEST_F(ExpressTest, sparseIndexIneligibility) {
    auto cq = canonicalize("{a: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{
        createIndexEntry(BSON("a" << 1), true /* unique */, true /* sparse */), false};
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        want.index,
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    // Sparse indexes are ineligible when matching against null.
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    cq = canonicalize("{a: null}");
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);


    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);
}

TEST_F(ExpressTest, nonBtreeEligibility) {
    auto cq = canonicalize("{a: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("$**" << 1), true),  // wildcard index is not a btree index type
        createIndexEntry(BSON("a" << 1), false /* unique */),
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);
}

TEST_F(ExpressTest, collationMismatchEligibility) {
    CollatorInterfaceMock revCollator(CollatorInterfaceMock::MockType::kReverseString);
    auto cq = canonicalize("{a: \"foo\"}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto revCollatorIdxEnt = createIndexEntry(BSON("a" << 1), true /* unique */);
    revCollatorIdxEnt.collator = &revCollator;

    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("a" << 1), false /* unique */),
        revCollatorIdxEnt,
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);
}

TEST_F(ExpressTest, partialIndexEligibility) {
    auto cq = canonicalize("{a: 1}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto partialIdxEnt = createIndexEntry(BSON("a" << 1), true /* unique */);
    auto idxExpr = canonicalize("{a: {$exists: true}}");
    partialIdxEnt.filterExpr = idxExpr->getPrimaryMatchExpression();

    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("a" << 1), false /* unique */),
        partialIdxEnt,
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent->index, partialIdxEnt);

    // Partial index is ineligible if query is NOT a subset of index filter expr.
    idxExpr = canonicalize("{b:  {$exists: true}}");
    params.mainCollectionInfo.indexes[1].filterExpr = idxExpr->getPrimaryMatchExpression();
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);
}

TEST_F(ExpressTest, coveringIndexEligibilityWithUniqueIndex) {
    auto cq = canonicalize("{a: 2}", false /*setLimit1*/, "{_id: 0, a: 1, c: 1}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    IndexForExpressEquality want{createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1), false),
                                 true};
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("a" << 1), true),
        createIndexEntry(BSON("a" << 1 << "b"
                                  << "1" << "c" << 1 << "d" << 1),
                         false),
        want.index,
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);
}

TEST_F(ExpressTest, coveringIndexEligibilityWithoutUniqueIndex) {
    auto cqNoLimit = canonicalize("{a: 2}", false /*setLimit1*/, "{_id: 0, a: 1, c: 1}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1), false),
                                 true};
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("a" << 1), false),
        createIndexEntry(BSON("a" << 1 << "b"
                                  << "1" << "c" << 1 << "d" << 1),
                         false),
        want.index,
    };
    auto ent = getIndexForExpressEquality(*cqNoLimit, params);
    ASSERT_EQUALS(ent, boost::none);

    auto cqLimit = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, a: 1, c: 1}");
    ent = getIndexForExpressEquality(*cqLimit, params);
    ASSERT_EQUALS(ent, want);

    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cqLimit, params);
    ASSERT_EQUALS(ent, boost::none);
}

TEST_F(ExpressTest, coveringIndexEligibilityWithCollation) {
    CollatorInterfaceMock revCollator(CollatorInterfaceMock::MockType::kReverseString);

    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality wantCovered{createIndexEntry(BSON("a" << 1), false), true};
    IndexForExpressEquality wantNotCovered{createIndexEntry(BSON("a" << 1), false), false};


    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("a" << 1 << "b"
                                  << "1" << "c" << 1 << "d" << 1),
                         false),
        wantNotCovered.index,
    };

    for (auto& indexEntry : params.mainCollectionInfo.indexes) {
        indexEntry.collator = &revCollator;
    }

    {
        auto cq = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, a: 1, c: 1}");
        cq->setCollator(std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kReverseString));

        auto ent = getIndexForExpressEquality(*cq, params);
        // Can't cover because "c" might be a string
        ASSERT_EQUALS(ent, wantNotCovered);
    }

    {
        auto cq = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, c: 1}");
        cq->setCollator(std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kReverseString));

        auto ent = getIndexForExpressEquality(*cq, params);
        // Can't cover because "c" might be a string
        ASSERT_EQUALS(ent, wantNotCovered);
    }

    {
        auto cq = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, a: 1}");
        cq->setCollator(std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kReverseString));

        auto ent = getIndexForExpressEquality(*cq, params);
        // Can cover because we know that "a" is not a string
        ASSERT_EQUALS(ent, wantCovered);
    }

    {
        auto cq = canonicalize("{a: \"abacaba\"}", true /*setLimit1*/, "{_id: 0, a: 1}");
        cq->setCollator(std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kReverseString));

        auto ent = getIndexForExpressEquality(*cq, params);
        // Can't cover because we know that "a" is a string
        ASSERT_EQUALS(ent, wantNotCovered);
    }
}

TEST_F(ExpressTest, coveringIndexEligibilityWithDuplicateKeys) {
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{
        createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "b" << 1), false), true};


    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("a" << 1), true),
        createIndexEntry(BSON("b" << 1 << "c" << 1 << "b" << 1), false),
        want.index,
    };

    auto cq = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, a: 1, b: 1}");
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);
}

TEST_F(ExpressTest, coveringIndexEligibilityWithWrongKeyOrder) {
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    IndexForExpressEquality want{createIndexEntry(BSON("a" << 1), true), false};

    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("b" << 1 << "a" << 1), false),
        want.index,
    };

    auto cq = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, a: 1, b: 1}");
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);
}

TEST_F(ExpressTest, coveringIndexEligibilityWithMultiKey) {
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;

    const auto cq = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, a: 1, c: 1}");

    {  // Can cover by a default index.
        auto indexEntry = createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1), false);
        params.mainCollectionInfo.indexes = {indexEntry};

        const auto ent = getIndexForExpressEquality(*cq, params);
        const IndexForExpressEquality want{indexEntry, true};
        ASSERT_EQUALS(ent, want);
    }
    {  // Can cover if unrelated field is multi-key.
        auto indexEntry = createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1), false);
        indexEntry.multikey = true;
        indexEntry.multikeyPaths = MultikeyPaths{{}, {0U}, {}};

        params.mainCollectionInfo.indexes = {indexEntry};
        auto ent = getIndexForExpressEquality(*cq, params);

        const IndexForExpressEquality want{indexEntry, true};
        ASSERT_EQUALS(ent, want);
    }
    {  // Can't cover if required field is multi-key.
        auto indexEntry = createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1), false);
        indexEntry.multikey = true;
        indexEntry.multikeyPaths = MultikeyPaths{{0U}, {}, {}};

        params.mainCollectionInfo.indexes = {indexEntry};
        auto ent = getIndexForExpressEquality(*cq, params);

        const IndexForExpressEquality want{indexEntry, false};
        ASSERT_EQUALS(ent, want);
    }
    {  // Can cover if filter field is multi-key and is not included into the projection.
        const auto cq = canonicalize("{a: 2}", true /*setLimit1*/, "{_id: 0, c: 1}");
        auto indexEntry = createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1), false);
        indexEntry.multikey = true;
        indexEntry.multikeyPaths = MultikeyPaths{{0U}, {}, {}};

        params.mainCollectionInfo.indexes = {indexEntry};
        auto ent = getIndexForExpressEquality(*cq, params);

        const IndexForExpressEquality want{indexEntry, true};
        ASSERT_EQUALS(ent, want);
    }
    {  // Can't cover if multikey paths are missing.
        auto indexEntry = createIndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1), false);
        indexEntry.multikey = true;

        params.mainCollectionInfo.indexes = {indexEntry};
        auto ent = getIndexForExpressEquality(*cq, params);

        const IndexForExpressEquality want{indexEntry, false};
        ASSERT_EQUALS(ent, want);
    }
}

TEST_F(ExpressTest, coveringIndexEligibilityWithSubFields) {
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;

    const auto cq = canonicalize("{'a.b': 2}", true /*setLimit1*/, "{_id: 0, c: 1}");

    auto indexEntry = createIndexEntry(BSON("a.b" << 1 << "c" << 1), false);
    params.mainCollectionInfo.indexes = {indexEntry};

    const auto ent = getIndexForExpressEquality(*cq, params);
    const IndexForExpressEquality want{indexEntry, true};
    ASSERT_EQUALS(ent, want);
}

}  // namespace mongo
