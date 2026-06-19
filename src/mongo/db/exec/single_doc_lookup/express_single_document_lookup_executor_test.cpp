/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/mock_local_lookup_eligibility.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/router_role/routing_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {
namespace {

using LookupResult = SingleDocumentLookupExecutor::LookupResult;

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test", "express_lookup");

/**
 * Test acquirer whose acquireCollection() runs an injected callback that throws. Lets the exception
 * matrix be driven without a sharded fixture.
 */
class ThrowingCollectionAcquirer final : public CollectionAcquirer {
public:
    explicit ThrowingCollectionAcquirer(std::function<void()> thrower)
        : _thrower(std::move(thrower)) {}

    Handle acquireCollection(OperationContext*,
                             const NamespaceString&,
                             boost::optional<UUID>) override {
        _thrower();
        MONGO_UNREACHABLE;
    }

private:
    std::function<void()> _thrower;
};

class ExpressLookupTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kTestNss, CollectionOptions()));
    }

    boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
        return make_intrusive<ExpressionContextForTest>(operationContext(), kTestNss);
    }

    void insert(BSONObj doc) {
        std::vector<BSONObj> docs{doc};
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kTestNss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
    }

    // Express executor over the real collection (OnDemand acquirer) with the given eligibility.
    ExpressSingleDocumentLookupExecutor makeExecutor(
        std::unique_ptr<LocalLookupEligibility> eligibility) {
        return ExpressSingleDocumentLookupExecutor(std::make_unique<OnDemandCollectionAcquirer>(),
                                                   std::move(eligibility));
    }

    LookupResult lookup(ExpressSingleDocumentLookupExecutor& exec, const Document& documentKey) {
        return exec.performLookup(
            makeExpCtx(), kTestNss, /*collectionUUID*/ boost::none, documentKey, boost::none);
    }
};

using ExpressLookupDeathTest = ExpressLookupTest;

// --- Constructor invariants -------------------------------------------------------------------

DEATH_TEST_REGEX_F(ExpressLookupDeathTest, TassertsOnNullAcquirer, "Tripwire assertion.*12841300") {
    ExpressSingleDocumentLookupExecutor(nullptr, MockLocalLookupEligibility::makeAlwaysLocal());
}

DEATH_TEST_REGEX_F(ExpressLookupDeathTest,
                   TassertsOnNullEligibility,
                   "Tripwire assertion.*12841301") {
    ExpressSingleDocumentLookupExecutor(std::make_unique<OnDemandCollectionAcquirer>(), nullptr);
}

// --- Eligibility gating (C1.6 / C3.1) ---------------------------------------------------------

TEST_F(ExpressLookupTest, UnknownEligibilityReturnsNotHandled) {
    insert(BSON("_id" << 1));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysUnknown());
    auto result = lookup(exec, Document{{"_id", 1}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
}

// --- Happy paths (C1.1 / C1.3 / C3.2) ---------------------------------------------------------

TEST_F(ExpressLookupTest, FindsExistingDocument) {
    insert(BSON("_id" << 1 << "value" << "hello"));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 1}});

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_TRUE(result.document.has_value());
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 1 << "value" << "hello"));
}

TEST_F(ExpressLookupTest, AbsentDocumentReturnsHandledNotFound) {
    insert(BSON("_id" << 1));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 404}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// --- _id-only seek behaviour (C1.7 / C3.5) ----------------------------------------------------

TEST_F(ExpressLookupTest, DocumentKeyWithoutIdReturnsNotHandled) {
    insert(BSON("_id" << 1 << "sk" << 5));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"sk", 5}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
}

TEST_F(ExpressLookupTest, MultiFieldDocumentKeyMatchesById) {
    insert(BSON("_id" << 1 << "sk" << 5));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    // Sharded-shape documentKey {_id, sk}: must be served locally by matching _id alone.
    auto result = lookup(exec, Document{{"_id", 1}, {"sk", 5}});

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 1 << "sk" << 5));
}

TEST_F(ExpressLookupTest, IdOnlyLookupIgnoresStaleShardKeyInDocumentKey) {
    insert(BSON("_id" << 1 << "sk" << 1));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    // documentKey carries a stale shard-key value; the _id-only seek returns the on-disk document.
    auto result = lookup(exec, Document{{"_id", 1}, {"sk", 999}});

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 1 << "sk" << 1));
}

// --- Missing collection -----------------------------------------------------------------------

TEST_F(ExpressLookupTest, MissingCollectionReturnsHandledNotFound) {
    auto otherNss = NamespaceString::createNamespaceString_forTest("test", "does_not_exist");
    ExpressSingleDocumentLookupExecutor exec(std::make_unique<OnDemandCollectionAcquirer>(),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    auto result =
        exec.performLookup(makeExpCtx(), otherNss, boost::none, Document{{"_id", 1}}, boost::none);
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// --- Exception matrix (C1.4 / C3.3) -----------------------------------------------------------

TEST_F(ExpressLookupTest, NamespaceNotFoundReturnsHandledNotFound) {
    ExpressSingleDocumentLookupExecutor exec(std::make_unique<ThrowingCollectionAcquirer>([] {
                                                 uasserted(ErrorCodes::NamespaceNotFound,
                                                           "ns gone");
                                             }),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 1}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

TEST_F(ExpressLookupTest, ShardCannotRefreshDueToLocksHeldPropagates) {
    ExpressSingleDocumentLookupExecutor exec(
        std::make_unique<ThrowingCollectionAcquirer>(
            [] { uasserted(ShardCannotRefreshDueToLocksHeldInfo(kTestNss), "locks held"); }),
        MockLocalLookupEligibility::makeAlwaysLocal());
    ASSERT_THROWS_CODE(lookup(exec, Document{{"_id", 1}}),
                       DBException,
                       ErrorCodes::ShardCannotRefreshDueToLocksHeld);
}

TEST_F(ExpressLookupTest, UnhandledExceptionPropagates) {
    ExpressSingleDocumentLookupExecutor exec(std::make_unique<ThrowingCollectionAcquirer>([] {
                                                 uasserted(ErrorCodes::InternalError, "boom");
                                             }),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    ASSERT_THROWS_CODE(lookup(exec, Document{{"_id", 1}}), DBException, ErrorCodes::InternalError);
}

// CAR errors (StaleConfig etc.) propagate out of the body: the eligibility owns refresh + retry
// inside run(). The non-routing AlwaysLocal impl has no route() wrapper, so the error escapes
// performLookup; the routing-aware impl's refresh + retry is covered by
// ShardedEligibilityRouteTest.
TEST_F(ExpressLookupTest, StaleConfigPropagates) {
    ExpressSingleDocumentLookupExecutor exec(
        std::make_unique<ThrowingCollectionAcquirer>([] {
            uasserted(StaleConfigInfo(
                          kTestNss, ShardVersion::UNTRACKED(), boost::none, ShardId("shard0")),
                      "stale config");
        }),
        MockLocalLookupEligibility::makeAlwaysLocal());

    ASSERT_THROWS_CODE(lookup(exec, Document{{"_id", 1}}), DBException, ErrorCodes::StaleConfig);
}

// --- _id shapes and collation -----------------------------------------------------------------

TEST_F(ExpressLookupTest, ObjectValuedIdWithOperatorShapeMatchesByEquality) {
    // A DBRef-shaped _id: its first field name starts with '$'. The lookup must match it by
    // equality rather than misparsing it as a query operator.
    const BSONObj dbRefId = BSON("$ref" << "other" << "$id" << 1);
    insert(BSON("_id" << dbRefId));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());

    auto result = lookup(exec, Document{{"_id", dbRefId}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << dbRefId));
}

TEST_F(ExpressLookupTest, IdLookupUsesCollectionDefaultCollation) {
    // A collection whose _id index is encoded under a case-insensitive default collation. The
    // lookup must adopt that collation, or the _id seek computes simple-collation bounds and
    // misses.
    const auto ciNss = NamespaceString::createNamespaceString_forTest("test", "ci_collation");
    CollectionOptions options;
    options.collation = BSON("locale" << "en" << "strength" << 2);
    ASSERT_OK(storageInterface()->createCollection(operationContext(), ciNss, options));
    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), ciNss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        std::vector<BSONObj> docs{BSON("_id" << "abc")};
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
    }

    ExpressSingleDocumentLookupExecutor exec(std::make_unique<OnDemandCollectionAcquirer>(),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    // Same _id value with different case; under the collection's case-insensitive collation it must
    // still resolve to the stored document.
    auto result =
        exec.performLookup(make_intrusive<ExpressionContextForTest>(operationContext(), ciNss),
                           ciNss,
                           boost::none,
                           Document{{"_id", "ABC"_sd}},
                           boost::none);
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << "abc"));
}

}  // namespace
}  // namespace mongo::exec::agg
