/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"

#include <boost/intrusive_ptr.hpp>
namespace mongo {

namespace {

using boost::intrusive_ptr;

using ListSearchIndexesTest = AggregationContextFixture;

TEST_F(ListSearchIndexesTest, ShouldParseWithIdNameOrEmptyObject) {
    auto expCtx = getExpCtx();

    // Test parsing with an 'id' field
    auto specObj = BSON("$listSearchIndexes" << BSON("id" << "indexID"));
    intrusive_ptr<DocumentSource> result =
        DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), expCtx);
    ASSERT(dynamic_cast<DocumentSourceListSearchIndexes*>(result.get()));

    // Test parsing with an 'name' field.
    specObj = BSON("$listSearchIndexes" << BSON("name" << "indexName"));
    result = DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), expCtx);
    ASSERT(dynamic_cast<DocumentSourceListSearchIndexes*>(result.get()));

    // Test parsing with no fields.
    specObj = BSON("$listSearchIndexes" << BSONObj());
    result = DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), expCtx);
    ASSERT(dynamic_cast<DocumentSourceListSearchIndexes*>(result.get()));
}

TEST_F(ListSearchIndexesTest, ShouldFailToParse) {
    auto expCtx = getExpCtx();

    // Test parsing with an unknown field.
    auto specObj = BSON("$listSearchIndexes" << BSON("unknown" << "unknownValue"));
    ASSERT_THROWS_CODE(
        DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::IDLUnknownField);

    // Test parsing with not an object.
    specObj = BSON("$listSearchIndexes" << 1999);
    ASSERT_THROWS_CODE(
        DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(ListSearchIndexesTest, RedactsNameFieldCorrectly) {
    auto expCtx = getExpCtx();
    auto specObj = BSON("$listSearchIndexes" << BSON("name" << "indexName"));
    auto docSource =
        DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            $listSearchIndexes: {
                name: "HASH<indexName>"
            }
        })",
        redact(*docSource));
}

TEST_F(ListSearchIndexesTest, RedactsIDFieldCorrectly) {
    auto expCtx = getExpCtx();
    auto specObj = BSON("$listSearchIndexes" << BSON("id" << "indexID"));
    auto docSource =
        DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            $listSearchIndexes: {
                id: "HASH<indexID>"
            }
        })",
        redact(*docSource));
}

TEST_F(ListSearchIndexesTest, RedactsEmptyObjCorrectly) {
    auto expCtx = getExpCtx();
    auto specObj = BSON("$listSearchIndexes" << BSONObj());
    auto docSource =
        DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            $listSearchIndexes: {}
        })",
        redact(*docSource));
}

TEST_F(ListSearchIndexesTest, ErrorWhenCollDoesNotExistWithoutAtlas) {
    auto expCtx = getExpCtx();
    struct MockMongoInterface final : public StubMongoProcessInterface {
        bool isExpectedToExecuteQueries() override {
            return true;
        }
    };
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>());

    auto specObj = BSON("$listSearchIndexes" << BSONObj());

    ASSERT_THROWS_CODE(
        DocumentSourceListSearchIndexes::createFromBson(specObj.firstElement(), expCtx),
        AssertionException,
        ErrorCodes::SearchNotEnabled);
}

}  // namespace
}  // namespace mongo
