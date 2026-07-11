// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/find_key.h"

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_shape/query_shape_hash.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <absl/hash/hash.h>

namespace mongo::query_stats {

namespace {
static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

static constexpr auto collectionType = query_shape::CollectionType::kCollection;

class FindKeyTest : public ServiceContextTest {
public:
    static std::unique_ptr<const Key> makeFindKeyFromQuery(const BSONObj& filter) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(filter.getOwned());
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));
        return std::make_unique<FindKey>(
            expCtx,
            *parsedFind->findCommandRequest,
            std::make_unique<query_shape::FindCmdShape>(*parsedFind, expCtx),
            collectionType);
    }
};

TEST_F(FindKeyTest, SizeOfFindCmdComponents) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    auto query = BSON("query" << 1 << "xEquals" << 42);
    fcr->setFilter(query.getOwned());
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));
    auto findComponents = std::make_unique<FindCmdComponents>(*parsedFind->findCommandRequest);

    ASSERT_GTE(findComponents->size(), sizeof(SpecificKeyComponents) + 3 /*bools and HasField*/);
    ASSERT_LTE(findComponents->size(),
               sizeof(SpecificKeyComponents) + 8 /*bools, HasField, and padding*/);
}

TEST_F(FindKeyTest, EquivalentFindCmdComponentsSizes) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto query = BSON("query" << 1 << "xEquals" << 42);

    // Set different fields in the find commands.
    auto fcrCursorTimeout = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcrCursorTimeout->setFilter(query.getOwned());
    fcrCursorTimeout->setNoCursorTimeout(true);
    auto parsedFindCursorTimeout =
        uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcrCursorTimeout)}));
    auto findComponentsCursorTimeout =
        std::make_unique<FindCmdComponents>(*parsedFindCursorTimeout->findCommandRequest);

    auto fcrAllowPartial = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcrAllowPartial->setFilter(query.getOwned());
    fcrAllowPartial->setAllowPartialResults(true);
    auto parsedFindAllowPartial =
        uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcrAllowPartial)}));
    auto findComponentsAllowPartial =
        std::make_unique<FindCmdComponents>(*parsedFindAllowPartial->findCommandRequest);

    ASSERT_EQ(findComponentsCursorTimeout->size(), findComponentsAllowPartial->size());
}

// Testing item from opCtx that should impact key size.
TEST_F(FindKeyTest, SizeOfFindKeyWithAndWithoutComment) {
    auto query = BSON("query" << 1 << "xEquals" << 42);

    auto keyWithoutComment = makeFindKeyFromQuery(query);

    auto opCtx = makeOperationContext();
    auto fcrWithComment = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcrWithComment->setFilter(query.getOwned());
    opCtx->setComment(BSON("comment" << " foo"));
    auto expCtxWithComment =
        ExpressionContextBuilder{}.fromRequest(opCtx.get(), *fcrWithComment).build();
    auto parsedFindWithComment =
        uassertStatusOK(parsed_find_command::parse(expCtxWithComment, {std::move(fcrWithComment)}));
    auto findShape =
        std::make_unique<query_shape::FindCmdShape>(*parsedFindWithComment, expCtxWithComment);
    auto keyWithComment =
        std::make_unique<query_stats::FindKey>(expCtxWithComment,
                                               *parsedFindWithComment->findCommandRequest,
                                               std::move(findShape),
                                               collectionType);

    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

// Testing item from command request that should impact key size.
TEST_F(FindKeyTest, SizeOfFindKeyWithAndWithoutReadConcern) {
    auto query = BSON("query" << 1 << "xEquals" << 42);

    auto keyWithoutReadConcern = makeFindKeyFromQuery(query);

    auto expCtxWithReadConcern = make_intrusive<ExpressionContextForTest>();
    auto fcrWithReadConcern = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcrWithReadConcern->setFilter(query.getOwned());
    fcrWithReadConcern->setReadConcern(repl::ReadConcernArgs::kLocal);
    auto parsedFindWithReadConcern = uassertStatusOK(
        parsed_find_command::parse(expCtxWithReadConcern, {std::move(fcrWithReadConcern)}));
    auto findShape = std::make_unique<query_shape::FindCmdShape>(*parsedFindWithReadConcern,
                                                                 expCtxWithReadConcern);
    auto keyWithReadConcern =
        std::make_unique<query_stats::FindKey>(expCtxWithReadConcern,
                                               *parsedFindWithReadConcern->findCommandRequest,
                                               std::move(findShape),
                                               collectionType);

    ASSERT_LT(keyWithoutReadConcern->size(), keyWithReadConcern->size());
}


TEST_F(FindKeyTest, OriginalQueryShapeHashAppearsInKey) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcr->setFilter(BSON("x" << 1));
    const auto hash = query_shape::QueryShapeHash::fromHexString(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    fcr->setOriginalQueryShapeHash(hash);
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));
    auto key =
        std::make_unique<FindKey>(expCtx,
                                  *parsedFind->findCommandRequest,
                                  std::make_unique<query_shape::FindCmdShape>(*parsedFind, expCtx),
                                  collectionType);

    const auto keyBson = key->toBson(
        expCtx->getOperationContext(),
        query_shape::SerializationOptions(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions),
        {});
    ASSERT_EQ(keyBson["originalQueryShapeHash"].str(),
              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
}

TEST_F(FindKeyTest, OriginalQueryShapeHashAbsentWhenNotSet) {
    const auto key = makeFindKeyFromQuery(BSON("x" << 1));
    const auto keyBson = key->toBson(
        make_intrusive<ExpressionContextForTest>()->getOperationContext(),
        query_shape::SerializationOptions(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions),
        {});
    ASSERT_TRUE(keyBson["originalQueryShapeHash"].eoo());
}

TEST_F(FindKeyTest, DifferentOriginalQueryShapeHashesProduceDifferentKeys) {
    auto makeKeyWithHash = [](std::string_view hexHash) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(BSON("x" << 1));
        fcr->setOriginalQueryShapeHash(query_shape::QueryShapeHash::fromHexString(hexHash));
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));
        return std::make_unique<FindKey>(
            expCtx,
            *parsedFind->findCommandRequest,
            std::make_unique<query_shape::FindCmdShape>(*parsedFind, expCtx),
            collectionType);
    };

    auto keyA = makeKeyWithHash("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    auto keyB = makeKeyWithHash("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    ASSERT_NE(absl::HashOf(*keyA), absl::HashOf(*keyB));
}

}  // namespace
}  // namespace mongo::query_stats
