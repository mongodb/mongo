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

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_impl.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_mock.h"
#include "mongo/db/exec/single_doc_lookup/mock_local_lookup_eligibility.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo::exec::agg {
namespace {

using Local = LocalLookupEligibility::Local;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db", "coll");

// Extracts the eligibility Decision via the run() combinator (check() was replaced by run()). run()
// returns the body's LookupResult, so the body captures the decision by reference and returns a
// throwaway result.
LocalLookupEligibility::Decision decisionOf(const LocalLookupEligibility& eligibility,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            const NamespaceString& nss,
                                            const Document& documentKey) {
    boost::optional<LocalLookupEligibility::Decision> captured;
    eligibility.run(expCtx,
                    nss,
                    documentKey,
                    [&](const LocalLookupEligibility::Decision& d)
                        -> SingleDocumentLookupExecutor::LookupResult {
                        captured = d;
                        return {};
                    });
    return *captured;
}

class LocalLookupEligibilityTest : public unittest::Test {
protected:
    boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
        return make_intrusive<ExpressionContextForTest>(_opCtx.get(), kNss);
    }

    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx = _serviceContext.makeOperationContext();
};

// --- AlwaysLocalEligibility -------------------------------------------------------------------

TEST_F(LocalLookupEligibilityTest, AlwaysLocalReturnsLocalWithoutVersions) {
    AlwaysLocalEligibility eligibility;
    auto decision = decisionOf(eligibility, makeExpCtx(), kNss, Document{{"_id", 1}});

    ASSERT_TRUE(LocalLookupEligibility::isLocal(decision));
    const auto& local = std::get<Local>(decision);
    ASSERT_FALSE(local.shardVersion.has_value());
    ASSERT_FALSE(local.dbVersion.has_value());
}

// --- AlwaysUnknownEligibility (sharded placeholder) -------------------------------------------

TEST_F(LocalLookupEligibilityTest, AlwaysUnknownDeclines) {
    AlwaysUnknownEligibility eligibility;
    auto decision = decisionOf(eligibility, makeExpCtx(), kNss, Document{{"_id", 1}});
    ASSERT_FALSE(LocalLookupEligibility::isLocal(decision));
}

// --- createScopedShardRole --------------------------------------------------------------------

TEST_F(LocalLookupEligibilityTest, CreateScopedShardRoleReturnsNulloptWhenNoVersion) {
    auto scope = createScopedShardRole(_opCtx.get(), kNss, Local{});
    ASSERT_FALSE(scope.has_value());
}

TEST_F(LocalLookupEligibilityTest, CreateScopedShardRoleEngagesWhenDbVersionSet) {
    Local local{.shardVersion = boost::none,
                .dbVersion = DatabaseVersion{UUID::gen(), Timestamp(1, 1)}};
    auto scope = createScopedShardRole(_opCtx.get(), kNss, local);
    ASSERT_TRUE(scope.has_value());
}

TEST_F(LocalLookupEligibilityTest, CreateScopedShardRoleEngagesWhenShardVersionSet) {
    Local local{.shardVersion = ShardVersion::UNTRACKED(), .dbVersion = boost::none};
    auto scope = createScopedShardRole(_opCtx.get(), kNss, local);
    ASSERT_TRUE(scope.has_value());
}

// --- MockLocalLookupEligibility (the test double itself) --------------------------------------

TEST_F(LocalLookupEligibilityTest, MockMakeAlwaysLocalReturnsLocalAndRecordsCall) {
    auto mock = MockLocalLookupEligibility::makeAlwaysLocal();
    auto decision = decisionOf(*mock, makeExpCtx(), kNss, Document{{"_id", 7}});

    ASSERT_TRUE(LocalLookupEligibility::isLocal(decision));
    ASSERT_EQ(mock->callCount(), 1u);
    ASSERT_EQ(mock->calls().front().nss, kNss);
}

TEST_F(LocalLookupEligibilityTest, MockMakeAlwaysUnknownReturnsUnknown) {
    auto mock = MockLocalLookupEligibility::makeAlwaysUnknown();
    ASSERT_FALSE(
        LocalLookupEligibility::isLocal(decisionOf(*mock, makeExpCtx(), kNss, Document{})));
}

TEST_F(LocalLookupEligibilityTest, MockMakeAlwaysLocalWithVersionCarriesVersion) {
    auto mock = MockLocalLookupEligibility::makeAlwaysLocalWithVersion(ShardVersion::UNTRACKED());
    auto decision = decisionOf(*mock, makeExpCtx(), kNss, Document{});
    ASSERT_TRUE(LocalLookupEligibility::isLocal(decision));
    ASSERT_TRUE(std::get<Local>(decision).shardVersion.has_value());
}

TEST_F(LocalLookupEligibilityTest, MockRecordsAllCallsInOrder) {
    auto mock = MockLocalLookupEligibility::makeAlwaysUnknown();
    auto nssA = NamespaceString::createNamespaceString_forTest("db", "a");
    auto nssB = NamespaceString::createNamespaceString_forTest("db", "b");

    (void)decisionOf(*mock, makeExpCtx(), nssA, Document{{"_id", 1}});
    (void)decisionOf(*mock, makeExpCtx(), nssB, Document{{"_id", 2}});

    ASSERT_EQ(mock->callCount(), 2u);
    ASSERT_EQ(mock->calls()[0].nss, nssA);
    ASSERT_EQ(mock->calls()[1].nss, nssB);
}

// --- LocalLookupEligibilityFactoryMock --------------------------------------------------------

TEST_F(LocalLookupEligibilityTest, FactoryMockAlwaysLocalProducesLocalEligibility) {
    auto factory = LocalLookupEligibilityFactoryMock::makeAlwaysLocal();
    auto eligibility = factory->makeLocalLookupEligibility(_opCtx.get());
    ASSERT_TRUE(
        LocalLookupEligibility::isLocal(decisionOf(*eligibility, makeExpCtx(), kNss, Document{})));
}

TEST_F(LocalLookupEligibilityTest, FactoryMockAlwaysUnknownProducesDecliningEligibility) {
    auto factory = LocalLookupEligibilityFactoryMock::makeAlwaysUnknown();
    auto eligibility = factory->makeLocalLookupEligibility(_opCtx.get());
    ASSERT_FALSE(
        LocalLookupEligibility::isLocal(decisionOf(*eligibility, makeExpCtx(), kNss, Document{})));
}

// --- LocalLookupEligibilityFactoryImpl --------------------------------------------------------

TEST_F(LocalLookupEligibilityTest, FactoryImplProducesAlwaysLocalWhenNotSharded) {
    // QueryTestServiceContext does not enable ShardingState, so the production factory must fall
    // into the replica-set branch and always decide Local.
    LocalLookupEligibilityFactoryImpl factory;
    auto eligibility = factory.makeLocalLookupEligibility(_opCtx.get());
    ASSERT_TRUE(
        LocalLookupEligibility::isLocal(decisionOf(*eligibility, makeExpCtx(), kNss, Document{})));
}

}  // namespace
}  // namespace mongo::exec::agg
