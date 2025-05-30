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

#include "mongo/db/pipeline/document_source_query_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};

/**
 * Subclass AggregationContextFixture to set the ExpressionContext's namespace to 'admin' with
 * {aggregate: 1} by default, so that parsing tests other than those which validate the namespace do
 * not need to explicitly set it.
 */
class DocumentSourceQueryStatsTest : public AggregationContextFixture {
public:
    DocumentSourceQueryStatsTest()
        : AggregationContextFixture(
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin)) {}

    std::unique_ptr<const Key> makeFindKeyFromQuery(BSONObj filter) {
        auto fcr = std::make_unique<FindCommandRequest>(kDefaultTestNss);
        fcr->setFilter(filter.getOwned());
        auto parsedFind =
            uassertStatusOK(parsed_find_command::parse(getExpCtx(), {std::move(fcr)}));

        auto statusWithShape =
            shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedFind, getExpCtx());

        return std::make_unique<query_stats::FindKey>(getExpCtx(),
                                                      *parsedFind->findCommandRequest,
                                                      std::move(statusWithShape.getValue()),
                                                      query_shape::CollectionType::kCollection);
    }

    QueryStatsStore& setUpQueryStatsStore(unsigned numPartitions = 1) {
        QueryStatsStoreManager::get(getServiceContext()) = std::make_unique<QueryStatsStoreManager>(
            16 * 1024 * 1024 /* cacheSize */, numPartitions);
        return getQueryStatsStore(getOpCtx());
    }

    // Calls getNext() on the document source and validates the result. Returns the field name of
    // the filter.
    static std::string getNextAndValidateResult(boost::intrusive_ptr<exec::agg::Stage> stage,
                                                const StringMap<std::string>& keyHashes,
                                                const StringMap<std::string>& shapeHashes,
                                                bool shouldValidateFilterDataType = true) {
        auto result = stage->getNext();
        ASSERT_TRUE(result.isAdvanced());
        const auto& doc = result.getDocument();

        const auto& filter = doc.getNestedField({"key.queryShape.filter"});
        ASSERT_FALSE(filter.missing());

        auto filterObj = filter.getDocument().toBson().firstElement();
        ASSERT_TRUE(keyHashes.contains(filterObj.fieldNameStringData()));
        if (shouldValidateFilterDataType) {
            ASSERT_BSONOBJ_EQ(filterObj.Obj(), BSON("$eq" << "?number"));
        }

        ASSERT_EQ(keyHashes.find(filterObj.fieldNameStringData())->second,
                  doc["keyHash"].getString());
        ASSERT_EQ(shapeHashes.find(filterObj.fieldNameStringData())->second,
                  doc["queryShapeHash"].getString());
        ASSERT_TRUE(doc["metrics"].isObject());
        ASSERT_TRUE(doc["asOf"].coercibleToDate());

        return std::string(filterObj.fieldName());
    }

    static const BSONObj kQueryStatsStage;
    static const BSONObj kQueryStatsStageWithTransformIdentifiers;
};

const BSONObj DocumentSourceQueryStatsTest::kQueryStatsStage = fromjson(R"({
        $queryStats: {}
    })");
const BSONObj DocumentSourceQueryStatsTest::kQueryStatsStageWithTransformIdentifiers = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
                algorithm: "hmac-sha-256",
                hmacKey: {
                    $binary: "YW4gYXJiaXRyYXJ5IEhNQUNrZXkgZm9yIHRlc3Rpbmc=",
                    $type: "08"
                }
            }
        }
    })");


TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfSpecIsNotObject) {
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: 1}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfNotRunOnAdmin) {
    getExpCtx()->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo")));
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: {}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfNotRunWithAggregateOne) {
    getExpCtx()->setNamespaceString(NamespaceString::createNamespaceString_forTest("admin.foo"));
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: {}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfUnrecognisedParameterSpecified) {
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: {foo: true}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceQueryStatsTest, ParseAndSerialize) {
    const auto doc =
        DocumentSourceQueryStats::createFromBson(kQueryStatsStage.firstElement(), getExpCtx());
    const auto queryStatsOp = static_cast<DocumentSourceQueryStats*>(doc.get());
    const auto expected = Document{{"$queryStats", Document{}}};
    const auto serialized = queryStatsOp->serialize().getDocument();
    ASSERT_DOCUMENT_EQ(expected, serialized);

    // Also make sure that we can parse out own serialization output.

    ASSERT_DOES_NOT_THROW(
        DocumentSourceQueryStats::createFromBson(serialized.toBson().firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceQueryStatsTest, ParseAndSerializeShouldIncludeHmacKey) {
    const auto doc = DocumentSourceQueryStats::createFromBson(
        kQueryStatsStageWithTransformIdentifiers.firstElement(), getExpCtx());
    const auto queryStatsOp = static_cast<DocumentSourceQueryStats*>(doc.get());
    const auto expected =
        Document{{"$queryStats",
                  Document{{"transformIdentifiers",
                            Document{{"algorithm", "hmac-sha-256"_sd},
                                     {"hmacKey",
                                      BSONBinData("an arbitrary HMACkey for testing",
                                                  32,
                                                  BinDataType::Sensitive)}}}}}};
    const auto serialized = queryStatsOp->serialize().getDocument();
    ASSERT_DOCUMENT_EQ(serialized, expected);

    // Also make sure that we can parse out own serialization output.

    ASSERT_DOES_NOT_THROW(
        DocumentSourceQueryStats::createFromBson(serialized.toBson().firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfAlgorithmIsNotSupported) {
    auto obj = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
                algorithm: "randomalgo"
            }
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceQueryStatsTest,
       ShouldFailToParseIfTransformIdentifiersSpecifiedButEmptyAlgorithm) {
    auto obj = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
                algorithm: ""
            }
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceQueryStatsTest,
       ShouldFailToParseIfTransformIdentifiersSpecifiedButNoAlgorithm) {
    auto obj = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
            }
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceQueryStatsTest, GetNextOverMultiplePartitions) {
    // First, populate the query stats store. One partition will have 2 entries, one will have
    // 1, and one will have none.
    auto& queryStatsStore = setUpQueryStatsStore(3 /* numPartitions */);
    queryStatsStore.put(0, QueryStatsEntry{makeFindKeyFromQuery(BSON("a" << 5))});
    queryStatsStore.put(1, QueryStatsEntry{makeFindKeyFromQuery(BSON("b" << 10))});
    queryStatsStore.put(3, QueryStatsEntry{makeFindKeyFromQuery(BSON("c" << 15))});

    ASSERT_TRUE(queryStatsStore.getPartition(0)->hasKey(0));
    ASSERT_TRUE(queryStatsStore.getPartition(0)->hasKey(3));
    ASSERT_TRUE(queryStatsStore.getPartition(1)->hasKey(1));

    const auto source =
        DocumentSourceQueryStats::createFromBson(kQueryStatsStage.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStage(source);

    // We check the actual hardcoded hash values so that we will know if a change is made
    // that changes the key and/or shape hash.
    const StringMap<std::string> keyHashes{{"a", "rcAw0HeEygzLKDoWncyDu3y3Fu63yQUbdMUmjTTqn4Y="},
                                           {"b", "MHwLPP5jfQQKM2xl38KtvhP1Eqhcv/264K9gRSBDgII="},
                                           {"c", "bng9EWFW0+DLt0cGJKgdW00ibvEuCo3dt/eWsiy1HZ4="}};
    const StringMap<std::string> shapeHashes{
        {"a", "9DC8DC4E831B149EBC3EA97C8A47ABDEA65ED0FD134344E8C4716F91C75B10A7"},
        {"b", "71CEB83D63C9329D53E6ADDD8D48540E63B2ED4804302A45239EE8049301C21D"},
        {"c", "A7443DD02A7DA19E4F6E6BEBA912BC61C7B968FD3154D2074684AD50B3E7958C"}};

    StringSet filters;
    filters.emplace(getNextAndValidateResult(stage, keyHashes, shapeHashes));
    filters.emplace(getNextAndValidateResult(stage, keyHashes, shapeHashes));
    filters.emplace(getNextAndValidateResult(stage, keyHashes, shapeHashes));

    ASSERT_TRUE(stage->getNext().isEOF());

    // We should see three unique filters.
    ASSERT_EQ(filters.size(), 3);
}

TEST_F(DocumentSourceQueryStatsTest, GetNextTransformIdentifiers) {
    // First, populate the query stats store. Both entries will be in the single store partition.
    auto& queryStatsStore = setUpQueryStatsStore();
    queryStatsStore.put(0, QueryStatsEntry{makeFindKeyFromQuery(BSON("a" << 5))});
    queryStatsStore.put(1, QueryStatsEntry{makeFindKeyFromQuery(BSON("b" << 10))});

    const auto source = DocumentSourceQueryStats::createFromBson(
        kQueryStatsStageWithTransformIdentifiers.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStage(source);

    // We check the actual hardcoded hash values so that we will know if a change is made
    // that changes the key and/or shape hash. Note that filter field names will be transformed.
    const StringMap<std::string> keyHashes{{"oJX9USpeTJc6jT5GPyTlKcljApSRarLEBFi+HN6ey0c=",
                                            "rcAw0HeEygzLKDoWncyDu3y3Fu63yQUbdMUmjTTqn4Y="},
                                           {"rxkaqVrbPs0OS5NIAEauEV68YIjfXKZ98wq3DcEsCPU=",
                                            "MHwLPP5jfQQKM2xl38KtvhP1Eqhcv/264K9gRSBDgII="}};
    const StringMap<std::string> shapeHashes{
        {"oJX9USpeTJc6jT5GPyTlKcljApSRarLEBFi+HN6ey0c=",
         "9DC8DC4E831B149EBC3EA97C8A47ABDEA65ED0FD134344E8C4716F91C75B10A7"},
        {"rxkaqVrbPs0OS5NIAEauEV68YIjfXKZ98wq3DcEsCPU=",
         "71CEB83D63C9329D53E6ADDD8D48540E63B2ED4804302A45239EE8049301C21D"}};

    StringSet filters;
    filters.emplace(getNextAndValidateResult(stage, keyHashes, shapeHashes));
    filters.emplace(getNextAndValidateResult(stage, keyHashes, shapeHashes));

    ASSERT_TRUE(stage->getNext().isEOF());

    // We should see two unique filters.
    ASSERT_EQ(filters.size(), 2);
}

TEST_F(DocumentSourceQueryStatsTest, GetNextKeyFailsToReParse) {
    // Manually construct a match expression that will successfully be constructed and serialized,
    // but be unable to re-parse the serialized representative value.
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(
        getExpCtx(), {std::make_unique<FindCommandRequest>(kDefaultTestNss)}));
    parsedFind->filter = std::make_unique<LTEMatchExpression>("a"_sd, Value(BSONRegEx(".*")));

    auto statusWithShape =
        shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedFind, getExpCtx());

    auto findKey = std::make_unique<query_stats::FindKey>(getExpCtx(),
                                                          *parsedFind->findCommandRequest,
                                                          std::move(statusWithShape.getValue()),
                                                          query_shape::CollectionType::kCollection);

    // Populate the query stats store with an entry that fails to re-parse.
    auto& queryStatsStore = setUpQueryStatsStore();
    queryStatsStore.put(0, QueryStatsEntry{std::move(findKey)});

    // First case - don't treat errors as fatal.
    internalQueryStatsErrorsAreCommandFatal.store(false);

    // Skip this check for debug builds because errors are always fatal in that environment.
    if (!kDebugBuild) {
        const auto source =
            DocumentSourceQueryStats::createFromBson(kQueryStatsStage.firstElement(), getExpCtx());
        auto stage = exec::agg::buildStage(source);
        // We should absorb the error and skip the record.
        ASSERT_TRUE(stage->getNext().isEOF());
    }

    // Now make sure that errors are propagated when the knob is set.
    internalQueryStatsErrorsAreCommandFatal.store(true);

    const auto source =
        DocumentSourceQueryStats::createFromBson(kQueryStatsStage.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStage(source);
    // This should raise an user assertion.
    ASSERT_THROWS_CODE(stage->getNext(), DBException, ErrorCodes::QueryStatsFailedToRecord);
}

TEST_F(DocumentSourceQueryStatsTest, DataTypeHashConsistency) {
    // First, populate the query stats store. Include a constant of each representative data type so
    // that we have comprehensive coverage of hash consistency. (We don't have to cover every
    // distinct type or interesting values because they will get converted to representative values
    // at this first parse-and-serialize step.)
    auto& queryStatsStore = setUpQueryStatsStore();
    queryStatsStore.put(0, QueryStatsEntry{makeFindKeyFromQuery(BSON("number" << 5))});
    queryStatsStore.put(1, QueryStatsEntry{makeFindKeyFromQuery(BSON("string" << "hello"))});
    queryStatsStore.put(
        2, QueryStatsEntry{makeFindKeyFromQuery(BSON("obj" << BSON("c" << BSONObj())))});
    queryStatsStore.put(
        3,
        QueryStatsEntry{makeFindKeyFromQuery(BSON("array" << BSON_ARRAY(1 << "hello" << true)))});
    queryStatsStore.put(4,
                        QueryStatsEntry{makeFindKeyFromQuery(BSON(
                            "binData" << BSONBinData("hello", 5, BinDataType::BinDataGeneral)))});
    queryStatsStore.put(
        5, QueryStatsEntry{makeFindKeyFromQuery(BSON("oid" << OID("aaaaaaaaaaaaaaaaaaaaaaaa")))});
    queryStatsStore.put(6, QueryStatsEntry{makeFindKeyFromQuery(BSON("bool" << true))});
    queryStatsStore.put(7, QueryStatsEntry{makeFindKeyFromQuery(BSON("date" << Date_t::min()))});
    queryStatsStore.put(8, QueryStatsEntry{makeFindKeyFromQuery(BSON("null" << BSONNULL))});
    queryStatsStore.put(9, QueryStatsEntry{makeFindKeyFromQuery(BSON("regex" << BSONRegEx(".*")))});
    queryStatsStore.put(10,
                        QueryStatsEntry{makeFindKeyFromQuery(
                            BSON("dbRef" << BSONDBRef("ns", OID("aaaaaaaaaaaaaaaaaaaaaaaa"))))});
    queryStatsStore.put(11, QueryStatsEntry{makeFindKeyFromQuery(BSON("js" << BSONCode("code")))});
    queryStatsStore.put(12,
                        QueryStatsEntry{makeFindKeyFromQuery(BSON(
                            "jsScope" << BSONCodeWScope("code", BSON("c" << 1))))});  // CodeWScope
    queryStatsStore.put(
        13, QueryStatsEntry{makeFindKeyFromQuery(BSON("timestamp" << Timestamp::max()))});
    queryStatsStore.put(14, QueryStatsEntry{makeFindKeyFromQuery(BSON("minKey" << MINKEY))});
    queryStatsStore.put(15, QueryStatsEntry{makeFindKeyFromQuery(BSON("maxKey" << MAXKEY))});

    const auto source =
        DocumentSourceQueryStats::createFromBson(kQueryStatsStage.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStage(source);

    // We check the actual hardcoded hash values so that we will know if a change is made
    // that changes the key and/or shape hash.
    const StringMap<std::string> keyHashes{
        {"number", "WzMDIQN1hzuJy4Ge3P9j9TA4jA5pqXvB+/BkHSg10Oc="},
        {"string", "Ln96PQvWTZzf1gGVtCK92y17LoTjQC7Nn/36usBiMkE="},
        {"obj", "Se4MORLYsnoaVqZ4LrK+JHC1pf8WgzpUAHjjb5J70K8="},
        {"array", "pw2TPLGY646sUwq13ng1qf5E7WxLPOhMXL3hWmgHIeI="},
        {"binData", "OsNua9pn5Gn6HgSkjgiEupEy9/hA0wfe7AKbrdroudE="},
        {"oid", "1p2vlQCUn7haXxHaxv9EXme2c/xNGRT+tkFTdWR4epE="},
        {"bool", "jITG5OBPG8K5H3kAMgRZFd76DVKkMxAp3CKPWxFXRfY="},
        {"date", "YqURPuYKQjAhroanAMTERUB3blqa3Ai6e9w+5izA1E4="},
        {"null", "76uKB6Q3MWoMJ5lMgPEg/4tr0IusD1w6NvMvoGZ70a0="},
        {"regex", "GHXud0kRDl+kTxkHHqGasV3UTbJCPf7snBtV4zGW+N8="},
        {"dbRef", "viwDulP9tNMdhDW6SSRazRvZj/ktzkgjAEpkTLrLs3o="},
        {"js", "hK647XC7TkCzkDpLveW5bib8WK7OskCEf2iiP1WgF4Q="},
        {"jsScope", "hY+eZX/Xa8UxxDgipmnYiYjxBoY/MFbRH/gbtM0mb3o="},
        {"timestamp", "SRM9DIz1PTrMAarPXQy6WAGDY8RFN3AKxfyI1/G2Vp0="},
        {"minKey", "BwwdBTVLb/01eqKQsKQuxfYIY0773O7cGMb6NdXUApI="},
        {"maxKey", "RMrGVm3VArh47ykP/hTH8KBHkuJJbOxyVlJQl9qiOdY="},
    };
    const StringMap<std::string> shapeHashes{
        {"number", "D4AD9F2739D820C07283049A9C49EE37AFAC3A8B6CA8D4BDA78540D272BD0B9C"},
        {"string", "5BE46216F9382803055A7F416C0C9CD3E0F6A7BDEDD270B468D86577F94CE7D9"},
        {"obj", "AC6EEC57C8E488B47F64B81B1BBCB49F74B8FBFD5A562689C6CCD224E3CFB6B3"},
        {"array", "0E03C35961C7845F623B227D677A97F0FB9015A2015774C00F9972F08A80BDA9"},
        {"binData", "F5553C7E381E1848312404966CC4BD996154C88BD07E357123048879FCB0D3D2"},
        {"oid", "016A3D1382049766798F3D6FBA410095076F9397CB76CE26A3CEEAFAFCC68FA2"},
        {"bool", "71CBDC0A16A6CA3CD762962D30399E52AB7A95A9667EB1D4ECCAD162B2D831FE"},
        {"date", "6641C4874358A2DD0EC2EDC8106338F92008D24055E10AB3CD4DA68026A2CF55"},
        {"null", "7649391F6B0E32D8056CB8EFBF7EDE4459DA4A72205AD14A0D8C270353C8288A"},
        {"regex", "0F07E129C04CE7EE29DC140B079260F04B5AE14873CEF61B8D307952E3DA12C8"},
        {"dbRef", "AE3996AB52E876FEE38C91284345606A095183B4067FA80D7D0CA7862403F6D0"},
        {"js", "871F98028D449282E473F5D170D5C401511DA3DBD286510B665BD89C7F2F6FE8"},
        {"jsScope", "94474327A88C9007771F4B92DCA38C28FF6291C77CC8AFE0B2174324D838F343"},
        {"timestamp", "212879C902C49582623DCA8707F8424A620AA908AF5112E172C2F24303C0BBB4"},
        {"minKey", "35AED33E9F18A576CA0E8C7064D64D9DC73DEF2D1583E1A1015EBCE3AC07EA9D"},
        {"maxKey", "ED49807BAEC6EFD6A74986AE67A1E222CB44E9DAF4791AD9A8D0B868C4051868"},
    };

    StringSet filters;
    for (size_t i = 0; i < keyHashes.size(); ++i) {
        filters.emplace(getNextAndValidateResult(
            stage, keyHashes, shapeHashes, false /* shouldValidateFilterDataType */));
    }

    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_EQ(filters.size(), keyHashes.size());
}

}  // namespace
}  // namespace mongo
