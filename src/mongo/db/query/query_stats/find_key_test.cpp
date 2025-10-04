/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/find_key.h"

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

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


}  // namespace
}  // namespace mongo::query_stats
