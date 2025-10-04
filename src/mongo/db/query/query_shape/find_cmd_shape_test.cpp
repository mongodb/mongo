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

#include "mongo/db/query/query_shape/find_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_shape {

namespace {
/**
 * TODO this was stolen from another test. Time for a library?
 * Simplistic redaction strategy for testing which appends the field name to the prefix "REDACT_".
 */
std::string applyHmacForTest(StringData sd) {
    return "REDACT_" + std::string{sd};
}

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};

struct RequestOptions {
    OptionalBool singleBatch = {};
    OptionalBool allowDiskUse = {};
    OptionalBool returnKey = {};
    OptionalBool showRecordId = {};
    OptionalBool tailable = {};
    OptionalBool awaitData = {};
    OptionalBool mirrored = {};
    OptionalBool limit = {};
    OptionalBool skip = {};
};
class FindCmdShapeTest : public ServiceContextTest {
public:
    void setUp() final {
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    std::unique_ptr<FindCmdShape> makeShapeFromSort(StringData sortJson) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setSort(fromjson(sortJson));
        auto&& parsedRequest =
            uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
        return std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    }

    const FindCmdShapeComponents& getShapeComponents(FindCmdShape& shape) {
        return static_cast<const FindCmdShapeComponents&>(shape.specificComponents());
    }

    BSONObj sortShape(StringData sortJson) {
        auto shape = makeShapeFromSort(sortJson);
        return getShapeComponents(*shape).sort;
    }

    /**
     * Returns the shape of the input sort, or boost::none if the input shape was a natural sort
     * which got converted into a hint.
     */
    boost::optional<BSONObj> maybeRedactedSortShape(StringData sortJson) {
        auto shape = makeShapeFromSort(sortJson);
        SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
        opts.transformIdentifiers = true;
        opts.transformIdentifiersCallback = applyHmacForTest;
        auto shapeBson = shape->toBson(
            _expCtx->getOperationContext(), opts, SerializationContext::stateDefault());
        if (auto sortElem = shapeBson["sort"]; !sortElem.eoo()) {
            return sortElem.Obj().getOwned();
        }
        return boost::none;
    }

    BSONObj redactedSortShape(StringData sortJson) {
        return *maybeRedactedSortShape(sortJson);
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    std::unique_ptr<FindCmdShapeComponents> makeShapeComponentsFromFilter(
        BSONObj filter, const RequestOptions& requestOptions = {}) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(filter.getOwned());
        fcr->setSingleBatch(requestOptions.singleBatch);
        fcr->setAllowDiskUse(requestOptions.allowDiskUse);
        fcr->setReturnKey(requestOptions.returnKey);
        fcr->setAllowDiskUse(requestOptions.showRecordId);
        fcr->setTailable(requestOptions.tailable);
        fcr->setAwaitData(requestOptions.awaitData);
        fcr->setMirrored(requestOptions.mirrored);
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(fcr)}));
        return std::make_unique<FindCmdShapeComponents>(*parsedFind, _expCtx);
    }

    std::unique_ptr<FindCmdShape> makeShapeFromFilter(const BSONObj& filter) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(filter.getOwned());
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(fcr)}));
        return std::make_unique<FindCmdShape>(*parsedFind, _expCtx);
    }
};

TEST_F(FindCmdShapeTest, NormalSortPattern) {
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"a.b.c":1,"foo":-1})",
        sortShape(R"({"a.b.c": 1, "foo": -1})"));
}

TEST_F(FindCmdShapeTest, NaturalSortPattern) {
    // $natural sorts are interpreted as a hint. Hints are not part of the shape (but should show up
    // in the query stats key).
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({})",
        sortShape(R"({$natural: 1})"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({})",
        sortShape(R"({$natural: -1})"));
}

TEST_F(FindCmdShapeTest, NaturalSortPatternWithMeta) {
    ASSERT_THROWS_CODE(
        sortShape(R"({$natural: 1, x: {$meta: "textScore"}})"), DBException, ErrorCodes::BadValue);
}

TEST_F(FindCmdShapeTest, MetaPatternWithoutNatural) {
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"normal":1,"$computed1":{"$meta":"textScore"}})",
        sortShape(R"({normal: 1, x: {$meta: "textScore"}})"));
}

// Here we have one test to ensure that the redaction policy is accepted and applied in the
// query_shape utility, but there are more extensive redaction tests in sort_pattern_test.cpp
TEST_F(FindCmdShapeTest, RespectsRedactionPolicy) {
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"REDACT_normal":1,"REDACT_y":1})",
        redactedSortShape(R"({normal: 1, y: 1})"));

    // No need to redact $natural. Again, this will be interpreted as a hint, but this test is
    // interesting to ensure the $-prefix of $natural doesn't confuse us.
    ASSERT(!maybeRedactedSortShape(R"({$natural: 1})"));
}

TEST_F(FindCmdShapeTest, NoOptionalArguments) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    ASSERT_EQUALS(0, getShapeComponents(*cmdShape).optionalArgumentsEncoding());
}

TEST_F(FindCmdShapeTest, AllOptionalArgumentsSetToTrue) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    // Tailable can not be set together with 'singleBatch' option.
    fcr->setSingleBatch(false);
    fcr->setAllowDiskUse(true);
    fcr->setReturnKey(true);
    fcr->setShowRecordId(true);
    fcr->setTailable(true);
    fcr->setAwaitData(true);
    fcr->setMirrored(true);
    fcr->setOplogReplay(true);
    fcr->setLimit(1);
    fcr->setSkip(1);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    ASSERT_EQUALS(0x2FFFF, getShapeComponents(*cmdShape).optionalArgumentsEncoding());
}

TEST_F(FindCmdShapeTest, AllOptionalArgumentsSetToFalse) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcr->setSingleBatch(false);
    fcr->setAllowDiskUse(false);
    fcr->setReturnKey(false);
    fcr->setShowRecordId(false);
    fcr->setTailable(false);
    fcr->setAwaitData(false);
    fcr->setMirrored(false);
    fcr->setOplogReplay(false);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    ASSERT_EQUALS(0x2AAA8, getShapeComponents(*cmdShape).optionalArgumentsEncoding());
}


TEST_F(FindCmdShapeTest, SizeOfShapeComponents) {
    auto query = BSON("query" << 1 << "xEquals" << 42);
    auto findCmdComponent = makeShapeComponentsFromFilter(query.getOwned());
    const auto querySize = findCmdComponent->filter.objsize();

    const auto minimumSize = sizeof(FindCmdShapeComponents) + querySize;
    ASSERT_GT(findCmdComponent->size(), minimumSize);
    ASSERT_LTE(findCmdComponent->size(),
               minimumSize + static_cast<size_t>(5 * BSONObj().objsize()));
}

TEST_F(FindCmdShapeTest, EquivalentShapeComponentsSizes) {
    auto query = BSON("query" << 1 << "xEquals" << 42);
    // Tailable can not be set together with 'singleBatch' option.
    auto mostlyTrueComponent = makeShapeComponentsFromFilter(query.getOwned(),
                                                             {.singleBatch = false,
                                                              .allowDiskUse = true,
                                                              .returnKey = true,
                                                              .showRecordId = true,
                                                              .tailable = true,
                                                              .awaitData = true,
                                                              .mirrored = true,
                                                              .limit = true,
                                                              .skip = true});

    auto mostlyFalseComponent = makeShapeComponentsFromFilter(query.getOwned(),
                                                              {.singleBatch = false,
                                                               .allowDiskUse = false,
                                                               .returnKey = false,
                                                               .showRecordId = false,
                                                               .tailable = true,
                                                               .awaitData = false,
                                                               .mirrored = false,
                                                               .limit = false,
                                                               .skip = false});

    ASSERT_EQ(mostlyTrueComponent->size(), mostlyFalseComponent->size());
}

TEST_F(FindCmdShapeTest, DifferentShapeComponentsSizes) {
    auto smallQuery = BSON("query" << BSONObj());
    auto smallFindCmdComponent = makeShapeComponentsFromFilter(smallQuery.getOwned());

    auto largeQuery = BSON("query" << 1 << "xEquals" << 42);
    auto largeFindCmdComponent = makeShapeComponentsFromFilter(largeQuery.getOwned());

    ASSERT_LT(smallQuery.objsize(), largeQuery.objsize());
    ASSERT_LT(smallFindCmdComponent->size(), largeFindCmdComponent->size());
}

TEST_F(FindCmdShapeTest, SizeOfShapeWithAndWithoutLet) {
    auto filter = BSON("query" << 1 << "xEquals" << 42);
    auto shapeWithoutLet = makeShapeFromFilter(filter.getOwned());

    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcr->setFilter(filter.getOwned());
    fcr->setLet(fromjson(R"({x: 4})"));
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto shapeWithLet = std::make_unique<FindCmdShape>(*parsedFind, _expCtx);

    ASSERT_LT(shapeWithoutLet->size(), shapeWithLet->size());
}

TEST_F(FindCmdShapeTest, SizeOfShapeWithAndWithoutCollation) {
    auto filter = BSON("query" << 1 << "xEquals" << 42);
    auto shapeWithoutCollation = makeShapeFromFilter(filter.getOwned());

    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcr->setFilter(filter.getOwned());
    fcr->setCollation(fromjson(R"({locale: "en_US"})"));
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto shapeWithCollation = std::make_unique<FindCmdShape>(*parsedFind, _expCtx);

    ASSERT_LT(shapeWithoutCollation->size(), shapeWithCollation->size());
}

TEST_F(FindCmdShapeTest, FindCommandShapeSHA256Hash) {
    auto makeTemplateFindCommandRequest = [](NamespaceStringOrUUID nsOrUUID) {
        auto findCommandRequest = std::make_unique<FindCommandRequest>(std::move(nsOrUUID));
        findCommandRequest->setFilter(BSON("a" << 1));
        findCommandRequest->setSort(BSON("b" << 1));
        findCommandRequest->setProjection(BSON("c" << 1));
        findCommandRequest->setCollation(BSON("locale" << "en_US"));
        findCommandRequest->setMin(BSON("d" << 1));
        findCommandRequest->setMax(BSON("d" << 9));
        findCommandRequest->setLet(BSON("e" << 1));
        findCommandRequest->setSingleBatch(false);
        findCommandRequest->setAllowDiskUse(true);
        findCommandRequest->setReturnKey(true);
        findCommandRequest->setShowRecordId(true);
        findCommandRequest->setMirrored(true);
        findCommandRequest->setOplogReplay(true);
        findCommandRequest->setLimit(1);
        findCommandRequest->setSkip(1);
        return findCommandRequest;
    };

    // Hexadecimal form of the SHA256 hash value of the template "find" command produced by
    // 'makeTemplateFindCommandRequest()'.
    const std::string templateHashValue{
        "5B498C8F35D92BCDD67A5192F37FFAACF2CA0BA49C68219C2088DE13F2BB2FFF"};

    // Verify value of the SHA256 hash of the "find" command shape - it must not change over
    // versions, and should not depend on the execution platform.
    {
        auto parsedFind = uassertStatusOK(
            parsed_find_command::parse(_expCtx, {makeTemplateFindCommandRequest(kDefaultTestNss)}));
        auto findCommandShape = std::make_unique<FindCmdShape>(*parsedFind, _expCtx);
        auto shapeHash = findCommandShape->sha256Hash(nullptr, SerializationContext{});
        ASSERT_EQ(templateHashValue, shapeHash.toHexString());
    }

    // Functions that modify a single component of the "find" command shape.
    using FindCommandRequestModificationFn = std::function<void(FindCommandRequest&)>;
    FindCommandRequestModificationFn modifyFilterFn = [](FindCommandRequest& findCommandRequest) {
        findCommandRequest.setFilter(BSONObj{});
    };
    FindCommandRequestModificationFn modifySortFn = [](FindCommandRequest& findCommandRequest) {
        findCommandRequest.setSort(BSONObj{});
    };
    FindCommandRequestModificationFn modifyProjectionFn =
        [](FindCommandRequest& findCommandRequest) {
            findCommandRequest.setProjection(BSONObj{});
        };
    FindCommandRequestModificationFn modifyCollationFn =
        [](FindCommandRequest& findCommandRequest) {
            findCommandRequest.setCollation(BSONObj{});
        };
    FindCommandRequestModificationFn modifyMinFn = [](FindCommandRequest& findCommandRequest) {
        findCommandRequest.setMin(BSONObj{});
    };
    FindCommandRequestModificationFn modifyMaxFn = [](FindCommandRequest& findCommandRequest) {
        findCommandRequest.setMax(BSONObj{});
    };
    FindCommandRequestModificationFn modifyLetFn = [](FindCommandRequest& findCommandRequest) {
        findCommandRequest.setLet(BSONObj{});
    };
    FindCommandRequestModificationFn modifyAttributesFn =
        [](FindCommandRequest& findCommandRequest) {
            // Modify one attribute - all boolean attributes are represented as one 32-bit word in
            // the serialized for hashing "find" command shape.
            findCommandRequest.setAllowDiskUse(false);
        };

    // Verify that all components of the "find" command shape are factored in when computing the
    // hash value. Modify components of template "find" command request and verify that the
    // resulting hash value differs.
    for (auto&& findCommandRequestModificationFunc : {modifyFilterFn,
                                                      modifySortFn,
                                                      modifyProjectionFn,
                                                      modifyCollationFn,
                                                      modifyMinFn,
                                                      modifyMaxFn,
                                                      modifyLetFn,
                                                      modifyAttributesFn}) {
        auto findCommandRequest = makeTemplateFindCommandRequest(kDefaultTestNss);
        findCommandRequestModificationFunc(*findCommandRequest);
        auto parsedFind =
            uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(findCommandRequest)}));
        auto findCommandShape = std::make_unique<FindCmdShape>(*parsedFind, _expCtx);
        auto shapeHash = findCommandShape->sha256Hash(nullptr, SerializationContext{});

        // Verify that the hash value is different from the hash value of the template "find"
        // command.
        ASSERT_NE(templateHashValue, shapeHash.toHexString())
            << findCommandShape->toFindCommandRequest()->toBSON().toString();
    }

    // Verify that the "find" command shape includes information if the collection of the command is
    // specified as a namespace or a collection UUID.
    NamespaceString testNamespace =
        NamespaceString::createNamespaceString_forTest("db.coll789ABCDEF");
    auto findCommandShapeHashForCollectionAsUUID = [&]() {
        // Build the collection UUID from the same byte array as the namespace.
        const auto collectionUUID = UUID::fromCDR(testNamespace.asDataRange());
        auto findCommandRequest = makeTemplateFindCommandRequest(NamespaceStringOrUUID{
            DatabaseName::createDatabaseName_forTest(boost::none, "any"), collectionUUID});
        auto parsedFind =
            uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(findCommandRequest)}));
        auto findCommandShape = std::make_unique<FindCmdShape>(*parsedFind, _expCtx);
        return findCommandShape->sha256Hash(nullptr, SerializationContext{});
    }();

    auto findCommandShapeHashForCollectionAsNamespace = [&]() {
        auto findCommandRequest =
            makeTemplateFindCommandRequest(NamespaceStringOrUUID{testNamespace});
        auto parsedFind =
            uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(findCommandRequest)}));
        auto findCommandShape = std::make_unique<FindCmdShape>(*parsedFind, _expCtx);
        return findCommandShape->sha256Hash(nullptr, SerializationContext{});
    }();

    ASSERT_NE(findCommandShapeHashForCollectionAsUUID.toHexString(),
              findCommandShapeHashForCollectionAsNamespace.toHexString());
}
}  // namespace

}  // namespace mongo::query_shape
