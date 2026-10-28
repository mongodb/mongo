// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/stats/internal_operation.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"

#include <absl/hash/hash.h>

namespace mongo::query_stats {

namespace {
static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

static constexpr auto collectionType = query_shape::CollectionType::kCollection;

class IsInternalTest : public ServiceContextTest {
public:
    static std::unique_ptr<FindKey> makeFindKey(OperationContext* opCtx) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kDefaultTestNss);
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(BSON("x" << 1));
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));
        return std::make_unique<FindKey>(
            expCtx,
            *parsedFind->findCommandRequest,
            std::make_unique<query_shape::FindCmdShape>(*parsedFind, expCtx),
            collectionType);
    }

    static BSONObj toBson(const FindKey& key) {
        return key.toBson(
            make_intrusive<ExpressionContextForTest>()->getOperationContext(),
            query_shape::SerializationOptions(
                query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions),
            {});
    }

    ServiceContext::UniqueOperationContext makeOpCtx(bool stamped) {
        auto opCtx = makeOperationContext();
        if (stamped) {
            markInternalOperation(opCtx.get());
        }
        return opCtx;
    }
};

// Keeping the field absent, rather than false, leaves the keyHash of user operations unchanged.
TEST_F(IsInternalTest, AbsentWhenNotStamped) {
    ASSERT_FALSE(toBson(*makeFindKey(makeOpCtx(/*stamped=*/false).get())).hasField("isInternal"));
}

TEST_F(IsInternalTest, PresentWhenStamped) {
    ASSERT_TRUE(
        toBson(*makeFindKey(makeOpCtx(/*stamped=*/true).get()))["isInternal"].booleanSafe());
}

// A command that arrives from another node, e.g. a shard request mongos sends to execute a user's
// command, is not internal just because its client is.
TEST_F(IsInternalTest, AbsentForInternalClientWithoutStamp) {
    auto client =
        getService()->makeClient("internalClient", transport::MockSession::create(nullptr));
    client->setIsInternalClient(true);
    auto opCtx = client->makeOperationContext();
    ASSERT_FALSE(toBson(*makeFindKey(opCtx.get())).hasField("isInternal"));
}

TEST_F(IsInternalTest, ProducesDifferentKeys) {
    const auto stampedHash = absl::HashOf(*makeFindKey(makeOpCtx(/*stamped=*/true).get()));
    ASSERT_NE(stampedHash, absl::HashOf(*makeFindKey(makeOpCtx(/*stamped=*/false).get())));
}

}  // namespace
}  // namespace mongo::query_stats
