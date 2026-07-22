// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/find_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::query_shape {

namespace {
/**
 * Simplistic redaction strategy for testing which appends the field name to the prefix "REDACT_".
 */
std::string applyHmacForTest(std::string_view sd) {
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
    OptionalBool rawData = {};
    OptionalBool limit = {};
    OptionalBool skip = {};
};
class FindCmdShapeTest : public ServiceContextTest {
public:
    void setUp() final {
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    std::unique_ptr<FindCmdShape> makeShapeFromSort(std::string_view sortJson) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setSort(fromjson(sortJson));
        auto&& parsedRequest =
            uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
        return std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    }

    const FindCmdShapeComponents& getShapeComponents(FindCmdShape& shape) {
        return static_cast<const FindCmdShapeComponents&>(shape.specificComponents());
    }

    BSONObj sortShape(std::string_view sortJson) {
        auto shape = makeShapeFromSort(sortJson);
        return getShapeComponents(*shape).sort;
    }

    /**
     * Returns the shape of the input sort, or boost::none if the input shape was a natural sort
     * which got converted into a hint.
     */
    boost::optional<BSONObj> maybeRedactedSortShape(std::string_view sortJson) {
        auto shape = makeShapeFromSort(sortJson);
        query_shape::SerializationOptions opts =
            query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions;
        opts.transformIdentifiers = true;
        opts.transformIdentifiersCallback = applyHmacForTest;
        auto shapeBson = shape->toBson(
            _expCtx->getOperationContext(), opts, SerializationContext::stateDefault());
        if (auto sortElem = shapeBson["sort"]; !sortElem.eoo()) {
            return sortElem.Obj().getOwned();
        }
        return boost::none;
    }

    BSONObj redactedSortShape(std::string_view sortJson) {
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
        fcr->setShowRecordId(requestOptions.showRecordId);
        fcr->setTailable(requestOptions.tailable);
        fcr->setAwaitData(requestOptions.awaitData);
        fcr->setMirrored(requestOptions.mirrored);
        fcr->setRawData(requestOptions.rawData);
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
    fcr->setRawData(true);
    fcr->setLimit(1);
    fcr->setSkip(1);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    // optionalArgumentsEncoding() uses 18 bits: 8 flags × 2 bits at positions 2-17 (the loop
    // shifts each flag left after ORing, so the first flag singleBatch lands at bits 16-17 and
    // the last flag oplogReplay at bits 2-3), plus limit at bit 1 and skip at bit 0.
    // rawData is NOT part of this value; it is encoded separately in sha256Hash.
    ASSERT_EQUALS((0b1011'1111'1111'1111 << 2) | 0b11,
                  getShapeComponents(*cmdShape).optionalArgumentsEncoding());
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
    fcr->setRawData(false);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);
    // 8 flags × 0b10 (false) = 16 bits, shifted left 2 for limit/skip = 18 bits. No skip/limit.
    ASSERT_EQUALS(0b1010101010101010 << 2,
                  getShapeComponents(*cmdShape).optionalArgumentsEncoding());
}

TEST_F(FindCmdShapeTest, RawDataTrueAppearsInShape) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcr->setRawData(true);
    auto&& parsedRequest =
        uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);

    ASSERT_TRUE(cmdShape->rawData);

    auto shapeBson =
        cmdShape->toBson(_expCtx->getOperationContext(),
                         SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                         SerializationContext::stateDefault());
    ASSERT_TRUE(shapeBson.hasField(FindCommandRequest::kRawDataFieldName));
    ASSERT_TRUE(shapeBson[FindCommandRequest::kRawDataFieldName].boolean());
}

TEST_F(FindCmdShapeTest, RawDataAbsentOrFalseNotInShape) {
    // rawData=false is normalized to absent: it does not change the query shape.
    for (auto rawDataVal : {boost::optional<bool>{}, boost::optional<bool>{false}}) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        if (rawDataVal.has_value()) {
            fcr->setRawData(*rawDataVal);
        }
        auto&& parsedRequest =
            uassertStatusOK(::mongo::parsed_find_command::parse(_expCtx, {std::move(fcr)}));
        auto cmdShape = std::make_unique<FindCmdShape>(*parsedRequest, _expCtx);

        ASSERT_FALSE(cmdShape->rawData);

        auto shapeBson =
            cmdShape->toBson(_expCtx->getOperationContext(),
                             SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                             SerializationContext::stateDefault());
        ASSERT_FALSE(shapeBson.hasField(FindCommandRequest::kRawDataFieldName));
    }
}

TEST_F(FindCmdShapeTest, RawDataDifferentiatesQueryShape) {
    auto makeShape = [&](boost::optional<bool> rawData) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(BSON("x" << 1));
        if (rawData.has_value()) {
            fcr->setRawData(*rawData);
        }
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(fcr)}));
        return std::make_unique<FindCmdShape>(*parsedFind, _expCtx);
    };

    auto shapeNoRawData = makeShape(boost::none);
    auto shapeRawDataTrue = makeShape(true);
    auto shapeRawDataFalse = makeShape(false);

    // rawData=absent and rawData=false should produce the same hash (false does not change shape).
    // rawData=true should be distinct from both.
    auto hashNone = shapeNoRawData->sha256Hash(nullptr, SerializationContext{});
    auto hashTrue = shapeRawDataTrue->sha256Hash(nullptr, SerializationContext{});
    auto hashFalse = shapeRawDataFalse->sha256Hash(nullptr, SerializationContext{});

    ASSERT_NE(hashNone.toHexString(), hashTrue.toHexString());
    ASSERT_EQ(hashNone.toHexString(), hashFalse.toHexString());
    ASSERT_NE(hashTrue.toHexString(), hashFalse.toHexString());
}

TEST_F(FindCmdShapeTest, RawDataPreservedInToFindCommandRequest) {
    auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
    fcr->setRawData(true);
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(_expCtx, {std::move(fcr)}));
    auto cmdShape = std::make_unique<FindCmdShape>(*parsedFind, _expCtx);

    auto roundTripped = cmdShape->toFindCommandRequest();
    ASSERT_TRUE(roundTripped->getRawData().has_value());
    ASSERT_TRUE(bool(roundTripped->getRawData()));
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
