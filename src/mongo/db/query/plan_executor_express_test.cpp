/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/plan_executor_express.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace {
std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr, bool setLimit1 = false) {
    auto nss = NamespaceString::createNamespaceString_forTest("test.collection");
    auto opCtx = OperationContext(nullptr, 5);
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));
    if (setLimit1) {
        findCommand->setLimit(1);
    }
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(&opCtx, *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
}

IndexEntry createIndexEntry(BSONObj keyPattern, bool unique, bool sparse = false) {
    return IndexEntry(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      IndexDescriptor::kLatestIndexVersion,
                      false /*multikey*/,
                      {} /*mutikeyPaths*/,
                      {} /*multikeyPathSet*/,
                      sparse,
                      unique,
                      IndexEntry::Identifier{"ident"},
                      nullptr /*filterExpr*/,
                      BSONObj() /*infoObj*/,
                      nullptr /*collatorInterface*/,
                      nullptr /*wildcardProjection*/);
}


}  // namespace

TEST(getIndexForExpressEquality, idIndexEligibility) {
    auto cq = canonicalize("{_id: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto want = createIndexEntry(BSON("_id" << 1), true);
    params.mainCollectionInfo.indexes = {createIndexEntry(BSON("_id" << 1), false),
                                         createIndexEntry(BSON("a" << 1), true),
                                         createIndexEntry(BSON("_id" << 1 << "f"
                                                                     << "1"),
                                                          true),
                                         want};
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    // Shard filtering shouldn't change anything since _id indexes are always unique.
    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);
}

TEST(getIndexForExpressEquality, compoundIdIndexEligibility) {
    auto cq = canonicalize("{_id: 2}", /* limit 1*/ true);
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto want = createIndexEntry(BSON("_id" << 1 << "f"
                                            << "1"),
                                 true);
    params.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1), true),
                                         createIndexEntry(BSON("_id" << 1 << "f"
                                                                     << "1"),
                                                          false),
                                         want};
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
TEST(getIndexForExpressEquality, singleFieldIndexEligibility) {
    auto cq = canonicalize("{a: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto want = createIndexEntry(BSON("a" << 1), true);
    params.mainCollectionInfo.indexes = {createIndexEntry(BSON("_id" << 1), true),
                                         createIndexEntry(BSON("a" << 1), false),
                                         createIndexEntry(BSON("a" << 1 << "f"
                                                                   << "1"),
                                                          true),
                                         want};
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);

    params.mainCollectionInfo.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, want);
}

// Non-unique single-field or compound indexes are ineligible without limit(1) and no
// shard filtering. The query must be guaranteed to produce at most 1 result.
TEST(getIndexForExpressEquality, nonUniqueIndexEligibility) {
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
    ASSERT_EQUALS(ent, params.mainCollectionInfo.indexes[1]);
}

TEST(getIndexForExpressEquality, compoundUniqueIndexEligibility) {
    auto cq = canonicalize("{a: 2}", true /* limit 1 */);
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto want = createIndexEntry(BSON("a" << 1 << "f"
                                          << "1"),
                                 true);
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        createIndexEntry(BSON("a" << 1), false),
        want,
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

TEST(getIndexForExpressEquality, sparseIndexIneligibility) {
    auto cq = canonicalize("{a: 2}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto want = createIndexEntry(BSON("a" << 1), true /* unique */, true /* sparse */);
    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("_id" << 1), true),
        want,
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

TEST(getIndexForExpressEquality, nonBtreeEligibility) {
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

TEST(getIndexForExpressEquality, collationMismatchEligibility) {
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

TEST(getIndexForExpressEquality, partialIndexEligibility) {
    auto cq = canonicalize("{a: 1}");
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
    auto partialIdxEnt = createIndexEntry(BSON("a" << 1), true /* unique */);
    auto idxExpr = canonicalize("{a: {$exists: true}}}");
    partialIdxEnt.filterExpr = idxExpr->getPrimaryMatchExpression();

    params.mainCollectionInfo.indexes = {
        createIndexEntry(BSON("a" << 1), false /* unique */),
        partialIdxEnt,
    };
    auto ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, partialIdxEnt);

    // Partial index is ineligible if query is NOT a subset of index filter expr.
    idxExpr = canonicalize("{b:  {$exists: true}}");
    params.mainCollectionInfo.indexes[1].filterExpr = idxExpr->getPrimaryMatchExpression();
    ent = getIndexForExpressEquality(*cq, params);
    ASSERT_EQUALS(ent, boost::none);
}

}  // namespace mongo
