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

#include "mongo/db/s/query_analysis_writer.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/platform/random.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <future>
#include <new>
#include <set>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("testDb", "testColl0");
const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("testDb", "testColl1");
const int samplesPerSecond = 100;

TEST(QueryAnalysisWriterBufferTest, AddBasic) {
    auto buffer = QueryAnalysisWriter::Buffer(nss0);

    auto doc0 = BSON("a" << 0);
    buffer.add(doc0);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc0.objsize());

    auto doc1 = BSON("a" << BSON_ARRAY(0 << 1 << 2));
    buffer.add(doc1);
    ASSERT_EQ(buffer.getCount(), 2);
    ASSERT_EQ(buffer.getSize(), doc0.objsize() + doc1.objsize());

    ASSERT_BSONOBJ_EQ(buffer.at(0), doc0);
    ASSERT_BSONOBJ_EQ(buffer.at(1), doc1);
}

TEST(QueryAnalysisWriterBufferTest, AddTooLarge) {
    auto buffer = QueryAnalysisWriter::Buffer(nss0);

    auto doc = BSON(std::string(BSONObjMaxUserSize, 'a') << 1);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 0);
    ASSERT_EQ(buffer.getSize(), 0);
}

TEST(QueryAnalysisWriterBufferTest, TruncateBasic) {
    auto testTruncateCommon = [](int oldCount, int newCount) {
        auto buffer = QueryAnalysisWriter::Buffer(nss0);

        std::vector<BSONObj> docs;
        for (auto i = 0; i < oldCount; i++) {
            docs.push_back(BSON("a" << i));
        }
        // The documents have the same size.
        auto docSize = docs.back().objsize();

        for (const auto& doc : docs) {
            buffer.add(doc);
        }
        ASSERT_EQ(buffer.getCount(), oldCount);
        ASSERT_EQ(buffer.getSize(), oldCount * docSize);

        buffer.truncate(newCount, (oldCount - newCount) * docSize);
        ASSERT_EQ(buffer.getCount(), newCount);
        ASSERT_EQ(buffer.getSize(), newCount * docSize);
        for (auto i = 0; i < newCount; i++) {
            ASSERT_BSONOBJ_EQ(buffer.at(i), docs[i]);
        }
    };

    testTruncateCommon(10 /* oldCount */, 6 /* newCount */);
    testTruncateCommon(10 /* oldCount */, 0 /* newCount */);  // Truncate all.
    testTruncateCommon(10 /* oldCount */, 9 /* newCount */);  // Truncate one.
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidIndex_Negative, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer(nss0);

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(-1, doc.objsize());
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidIndex_Positive, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer(nss0);

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(2, doc.objsize());
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidSize_Negative, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer(nss0);

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(0, -doc.objsize());
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidSize_Zero, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer(nss0);

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(0, 0);
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidSize_Positive, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer(nss0);

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(0, doc.objsize() * 2);
}

void assertBsonObjEqualUnordered(const BSONObj& lhs, const BSONObj& rhs) {
    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(comparator.compare(lhs, rhs), 0);
}

struct SampledReadQuery {
    UUID sampleId;
    BSONObj filter;
    BSONObj collation;
};

struct SampledDiff {
    UUID sampleId;
    BSONObj preImage;
    BSONObj postImage;
    BSONObj diff;
};

struct QueryAnalysisWriterTest : service_context_test::WithSetupTransportLayer,
                                 public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        QueryAnalysisWriter::get(operationContext())->onStartup(operationContext());

        DBDirectClient client(operationContext());
        client.createCollection(nss0);
        client.createCollection(nss1);

        auto& tracker = QueryAnalysisSampleTracker::get(operationContext());
        auto configuration0 = CollectionQueryAnalyzerConfiguration(
            nss0, getCollectionUUID(nss0), samplesPerSecond, Date_t::now());
        auto configuration1 = CollectionQueryAnalyzerConfiguration(
            nss1, getCollectionUUID(nss1), samplesPerSecond, Date_t::now());
        tracker.refreshConfigurations({configuration0, configuration1});
    }

    void tearDown() override {
        QueryAnalysisWriter::get(operationContext())->onShutdown();
        ShardServerTestFixture::tearDown();
    }

protected:
    // Use mock clock by default to prevent time from advancing during the test which would make
    // time related checks fail.
    QueryAnalysisWriterTest(bool useMockClock = true)
        : ShardServerTestFixture(Options{}.useMockClock(useMockClock),
                                 false /* setUpMajorityReads */) {}

    UUID getCollectionUUID(const NamespaceString& nss) const {
        auto collectionCatalog = CollectionCatalog::get(operationContext());
        return *collectionCatalog->lookupUUIDByNSS(operationContext(), nss);
    }

    BSONObj makeNonEmptyFilter() {
        return BSON("_id" << UUID::gen());
    }

    BSONObj makeNonEmptyCollation() {
        int strength = _getRandomInt(5) + 1;
        return BSON("locale" << "en_US"
                             << "strength" << strength);
    }

    BSONObj makeLetParameters() {
        return BSON("constant" << UUID::gen());
    }

    /*
     * Asserts that collection nss has a TTL index with the specified name and
     * expireAfterSeconds set to 0.
     */
    void assertTTLIndexExists(const NamespaceString& nss, const std::string& name) const {
        DBDirectClient client(operationContext());
        BSONObj result;
        client.runCommand(nss.dbName(), BSON("listIndexes" << nss.coll()), result);

        auto indexes = result.getObjectField("cursor").getField("firstBatch").Array();
        auto iter = indexes.begin();
        BSONObj indexSpec = iter->Obj();
        bool foundTTLIndex = false;

        while (iter != indexes.end()) {
            foundTTLIndex =
                (indexSpec.hasField("name") && indexSpec.getStringField("name") == name);
            if (foundTTLIndex) {
                break;
            }
            ++iter;
            indexSpec = iter->Obj();
        }

        ASSERT(foundTTLIndex);
        ASSERT_EQ(indexSpec.getObjectField("key").getIntField("expireAt"), 1);
        ASSERT_EQ(indexSpec.getIntField("expireAfterSeconds"), 0);
    }

    /*
     * Makes an UpdateCommandRequest for the collection 'nss' such that the command contains
     * 'numOps' updates and the ones whose indices are in 'markForSampling' are marked for sampling.
     * Returns the UpdateCommandRequest and the map storing the expected sampled
     * UpdateCommandRequests by sample id, if any.
     */
    std::pair<write_ops::UpdateCommandRequest, std::map<UUID, write_ops::UpdateCommandRequest>>
    makeUpdateCommandRequest(const NamespaceString& nss,
                             int numOps,
                             std::set<int> markForSampling,
                             std::string filterFieldName = "a") {
        write_ops::UpdateCommandRequest originalCmd(nss);
        std::vector<write_ops::UpdateOpEntry> updateOps;  // populated below.
        originalCmd.setLet(let);
        originalCmd.getWriteCommandRequestBase().setEncryptionInformation(encryptionInformation);

        std::map<UUID, write_ops::UpdateCommandRequest> expectedSampledCmds;

        for (auto i = 0; i < numOps; i++) {
            auto updateOp = write_ops::UpdateOpEntry(
                BSON(filterFieldName << i),
                write_ops::UpdateModification(BSON("$set" << BSON("b.$[element]" << i))));
            updateOp.setC(BSON("x" << i));
            updateOp.setArrayFilters(std::vector<BSONObj>{BSON("element" << BSON("$gt" << i))});
            updateOp.setSort(BSON("_id" << 1));
            updateOp.setMulti(_getRandomBool());
            updateOp.setUpsert(_getRandomBool());
            updateOp.setUpsertSupplied(_getRandomBool());
            updateOp.setCollation(makeNonEmptyCollation());

            if (markForSampling.find(i) != markForSampling.end()) {
                auto sampleId = UUID::gen();
                updateOp.setSampleId(sampleId);

                write_ops::UpdateCommandRequest expectedSampledCmd = originalCmd;
                expectedSampledCmd.setUpdates({updateOp});
                expectedSampledCmd.getWriteCommandRequestBase().setEncryptionInformation(
                    boost::none);
                expectedSampledCmds.emplace(sampleId, std::move(expectedSampledCmd));
            }
            updateOps.push_back(updateOp);
        }
        originalCmd.setUpdates(updateOps);

        return {originalCmd, expectedSampledCmds};
    }

    /*
     * Makes an DeleteCommandRequest for the collection 'nss' such that the command contains
     * 'numOps' deletes and the ones whose indices are in 'markForSampling' are marked for sampling.
     * Returns the DeleteCommandRequest and the map storing the expected sampled
     * DeleteCommandRequests by sample id, if any.
     */
    std::pair<write_ops::DeleteCommandRequest, std::map<UUID, write_ops::DeleteCommandRequest>>
    makeDeleteCommandRequest(const NamespaceString& nss,
                             int numOps,
                             std::set<int> markForSampling,
                             std::string filterFieldName = "a") {
        write_ops::DeleteCommandRequest originalCmd(nss);
        std::vector<write_ops::DeleteOpEntry> deleteOps;  // populated and set below.
        originalCmd.setLet(let);
        originalCmd.getWriteCommandRequestBase().setEncryptionInformation(encryptionInformation);

        std::map<UUID, write_ops::DeleteCommandRequest> expectedSampledCmds;

        for (auto i = 0; i < numOps; i++) {
            auto deleteOp =
                write_ops::DeleteOpEntry(BSON(filterFieldName << i), _getRandomBool() /* multi */);
            deleteOp.setCollation(makeNonEmptyCollation());

            if (markForSampling.find(i) != markForSampling.end()) {
                auto sampleId = UUID::gen();
                deleteOp.setSampleId(sampleId);

                write_ops::DeleteCommandRequest expectedSampledCmd = originalCmd;
                expectedSampledCmd.setDeletes({deleteOp});
                expectedSampledCmd.getWriteCommandRequestBase().setEncryptionInformation(
                    boost::none);
                expectedSampledCmds.emplace(sampleId, std::move(expectedSampledCmd));
            }
            deleteOps.push_back(deleteOp);
        }
        originalCmd.setDeletes(deleteOps);

        return {originalCmd, expectedSampledCmds};
    }

    /*
     * Makes a FindAndModifyCommandRequest for the collection 'nss'. The findAndModify is an update
     * if 'isUpdate' is true, and a remove otherwise. If 'markForSampling' is true, it is marked for
     * sampling. Returns the FindAndModifyCommandRequest and the map storing the expected sampled
     * FindAndModifyCommandRequests by sample id, if any.
     */
    std::pair<write_ops::FindAndModifyCommandRequest,
              std::map<UUID, write_ops::FindAndModifyCommandRequest>>
    makeFindAndModifyCommandRequest(const NamespaceString& nss,
                                    bool isUpdate,
                                    bool markForSampling,
                                    std::string filterFieldName = "a") {
        write_ops::FindAndModifyCommandRequest originalCmd(nss);
        originalCmd.setQuery(BSON(filterFieldName << 0));
        originalCmd.setUpdate(
            write_ops::UpdateModification(BSON("$set" << BSON("b.$[element]" << 0))));
        originalCmd.setArrayFilters(std::vector<BSONObj>{BSON("element" << BSON("$gt" << 10))});
        originalCmd.setSort(BSON("_id" << 1));
        if (isUpdate) {
            originalCmd.setUpsert(_getRandomBool());
            originalCmd.setNew(_getRandomBool());
        }
        originalCmd.setCollation(makeNonEmptyCollation());
        originalCmd.setLet(let);
        originalCmd.setEncryptionInformation(encryptionInformation);

        std::map<UUID, write_ops::FindAndModifyCommandRequest> expectedSampledCmds;
        if (markForSampling) {
            auto sampleId = UUID::gen();
            originalCmd.setSampleId(sampleId);

            auto expectedSampledCmd = originalCmd;
            expectedSampledCmd.setEncryptionInformation(boost::none);
            expectedSampledCmds.emplace(sampleId, std::move(expectedSampledCmd));
        }

        return {originalCmd, expectedSampledCmds};
    }

    void deleteSampledQueryDocuments() const {
        _deleteConfigDocuments(NamespaceString::kConfigSampledQueriesNamespace);
    }

    /**
     * Returns the number of the documents for the collection 'nss' in the config.sampledQueries
     * collection.
     */
    int getSampledQueryDocumentsCount(const NamespaceString& nss) {
        return _getConfigDocumentsCount(NamespaceString::kConfigSampledQueriesNamespace, nss);
    }

    /*
     * Asserts that there is a sampled read query document with the given sample id and that it has
     * the given fields.
     */
    void assertSampledReadQueryDocument(const UUID& sampleId,
                                        const NamespaceString& nss,
                                        SampledCommandNameEnum cmdName,
                                        const BSONObj& filter,
                                        const BSONObj& collation,
                                        const boost::optional<BSONObj>& letParameters = boost::none,
                                        const boost::optional<int>& expirationSecs = boost::none) {
        auto doc = _getConfigDocument(NamespaceString::kConfigSampledQueriesNamespace, sampleId);
        auto parsedQueryDoc =
            SampledQueryDocument::parse(IDLParserContext("QueryAnalysisWriterTest"), doc);

        ASSERT_EQ(parsedQueryDoc.getNs(), nss);
        ASSERT_EQ(parsedQueryDoc.getCollectionUuid(), getCollectionUUID(nss));
        ASSERT_EQ(parsedQueryDoc.getSampleId(), sampleId);
        ASSERT(parsedQueryDoc.getCmdName() == cmdName);
        auto parsedCmd = SampledReadCommand::parse(IDLParserContext("QueryAnalysisWriterTest"),
                                                   parsedQueryDoc.getCmd());
        ASSERT_BSONOBJ_EQ(parsedCmd.getFilter(), filter);
        ASSERT_BSONOBJ_EQ(parsedCmd.getCollation(), collation);

        if (letParameters) {
            ASSERT(parsedCmd.getLet().has_value());
            ASSERT_BSONOBJ_EQ(*parsedCmd.getLet(), *letParameters);
        } else {
            ASSERT(!parsedCmd.getLet().has_value());
        }

        if (expirationSecs) {
            ASSERT_EQ(parsedQueryDoc.getExpireAt(),
                      getServiceContext()->getFastClockSource()->now() + Seconds(*expirationSecs));
        }
    }

    /*
     * Asserts that there is a sampled write query document with the given sample id and that it has
     * the given fields.
     */
    template <typename CommandRequestType>
    void assertSampledWriteQueryDocument(const UUID& sampleId,
                                         const NamespaceString& nss,
                                         SampledCommandNameEnum cmdName,
                                         const CommandRequestType& expectedCmd,
                                         const boost::optional<int>& expirationSecs = boost::none) {
        auto doc = _getConfigDocument(NamespaceString::kConfigSampledQueriesNamespace, sampleId);
        auto parsedQueryDoc =
            SampledQueryDocument::parse(IDLParserContext("QueryAnalysisWriterTest"), doc);

        ASSERT_EQ(parsedQueryDoc.getNs(), nss);
        ASSERT_EQ(parsedQueryDoc.getCollectionUuid(), getCollectionUUID(nss));
        ASSERT_EQ(parsedQueryDoc.getSampleId(), sampleId);
        ASSERT(parsedQueryDoc.getCmdName() == cmdName);
        auto parsedCmd = CommandRequestType::parse(IDLParserContext("QueryAnalysisWriterTest"),
                                                   parsedQueryDoc.getCmd());
        ASSERT_BSONOBJ_EQ(parsedCmd.toBSON(), expectedCmd.toBSON());

        if (expirationSecs) {
            ASSERT_EQ(parsedQueryDoc.getExpireAt(),
                      getServiceContext()->getFastClockSource()->now() + Seconds(*expirationSecs));
        }
    }

    void deleteSampledDiffDocuments() const {
        _deleteConfigDocuments(NamespaceString::kConfigSampledQueriesDiffNamespace);
    }

    /*
     * Returns the number of the documents for the collection 'nss' in the config.sampledQueriesDiff
     * collection.
     */
    int getDiffDocumentsCount(const NamespaceString& nss) {
        return _getConfigDocumentsCount(NamespaceString::kConfigSampledQueriesDiffNamespace, nss);
    }

    /*
     * Asserts that there is a sampled diff document with the given sample id and that it has
     * the given fields.
     */
    void assertDiffDocument(const UUID& sampleId,
                            const NamespaceString& nss,
                            const BSONObj& expectedDiff) {
        auto doc =
            _getConfigDocument(NamespaceString::kConfigSampledQueriesDiffNamespace, sampleId);
        auto parsedDiffDoc =
            SampledQueryDiffDocument::parse(IDLParserContext("QueryAnalysisWriterTest"), doc);

        ASSERT_EQ(parsedDiffDoc.getNs(), nss);
        ASSERT_EQ(parsedDiffDoc.getCollectionUuid(), getCollectionUUID(nss));
        ASSERT_EQ(parsedDiffDoc.getSampleId(), sampleId);
        assertBsonObjEqualUnordered(parsedDiffDoc.getDiff(), expectedDiff);
    }

    /*
     * The helper for testing that samples are discarded.
     */
    void assertNoSampling(const NamespaceString& nss, const UUID& collUuid);

    /*
     * Turns on the failpoint to mock an error for the insert statement for the sample with the
     * given id. Returns the failpoint.
     */
    FailPoint* configureMockErrorInsertResponseFailPoint(const UUID& sampleId, int errorIndex) {
        auto failpoint =
            globalFailPointRegistry().find("queryAnalysisWriterMockInsertCommandResponse");
        failpoint->setMode(FailPoint::Mode::alwaysOn,
                           0,
                           BSON("_id" << sampleId << "errorDetails"
                                      << BSON("index" << errorIndex << "code"
                                                      << ErrorCodes::InternalError << "errmsg"
                                                      << "Mock error")));
        return failpoint;
    }

    // Test with both empty and non-empty filter and collation to verify that the
    // QueryAnalysisWriter doesn't require filter or collation to be non-empty.
    const BSONObj emptyFilter{};
    const BSONObj emptyCollation{};

    const BSONObj let = BSON("x" << 1);
    // Test with EncryptionInformation to verify that QueryAnalysisWriter does not persist the
    // WriteCommandRequestBase fields, especially this sensitive field.
    const EncryptionInformation encryptionInformation{BSON("foo" << "bar")};

    int oneSecondExpirationSecs = 1;
    int oneWeekExpirationSecs = 7 * 24 * 3600;
    int oneMonthExpirationSecs = 30 * 24 * 3600;
    int oneYearExpirationSecs = 365 * 24 * 3600;

private:
    int32_t _getRandomInt(int32_t max) {
        return _random.nextInt32(max);
    }

    bool _getRandomBool() {
        return _getRandomInt(2) == 0;
    }

    /**
     * Returns the number of the documents for the collection 'collNss' in the config collection
     * 'configNss'.
     */
    int _getConfigDocumentsCount(const NamespaceString& configNss,
                                 const NamespaceString& collNss) const {
        DBDirectClient client(operationContext());
        return client.count(configNss, BSON("ns" << collNss.toString_forTest()));
    }

    /**
     * Returns the document with the given _id in the config collection 'configNss'.
     */
    BSONObj _getConfigDocument(const NamespaceString configNss, const UUID& id) const {
        DBDirectClient client(operationContext());

        FindCommandRequest findRequest{configNss};
        findRequest.setFilter(BSON("_id" << id));
        auto cursor = client.find(std::move(findRequest));
        ASSERT(cursor->more());
        return cursor->next();
    }

    /**
     * Removes all the documents in the config collection 'configNss'.
     */
    void _deleteConfigDocuments(const NamespaceString configNss) const {
        DBDirectClient client(operationContext());
        client.remove(configNss, {} /* filter */, true /*removeMany=*/);
    }

    // This fixture manually flushes sampled queries and diffs.
    FailPointEnableBlock _fp{"disableQueryAnalysisWriterFlusher"};
    PseudoRandom _random{SecureRandom{}.nextInt64()};
};

TEST_F(QueryAnalysisWriterTest, CreateTTLIndexes) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto future = writer.createTTLIndexes(operationContext());
    future.get();
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesNamespace,
                         QueryAnalysisWriter::kSampledQueriesTTLIndexName);
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesDiffNamespace,
                         QueryAnalysisWriter::kSampledQueriesDiffTTLIndexName);
    assertTTLIndexExists(NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace,
                         QueryAnalysisWriter::kAnalyzeShardKeySplitPointsTTLIndexName);
}

TEST_F(QueryAnalysisWriterTest, CreateTTLIndexesWhenSampledQueriesIndexExists) {
    auto failCreateIndexes = globalFailPointRegistry().find("failCommand");
    failCreateIndexes->setMode(
        FailPoint::alwaysOn,
        0,
        BSON("failCommands" << BSON_ARRAY("createIndexes") << "namespace"
                            << NamespaceString::kConfigSampledQueriesNamespace.toStringForErrorMsg()
                            << "errorCode" << ErrorCodes::IndexAlreadyExists
                            << "failInternalCommands" << true << "failLocalClients" << true));
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto future = writer.createTTLIndexes(operationContext());
    future.get();
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesDiffNamespace,
                         QueryAnalysisWriter::kSampledQueriesDiffTTLIndexName);
    assertTTLIndexExists(NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace,
                         QueryAnalysisWriter::kAnalyzeShardKeySplitPointsTTLIndexName);
}

TEST_F(QueryAnalysisWriterTest, CreateTTLIndexesWhenSampledQueriesDiffIndexExists) {
    auto failCreateIndexes = globalFailPointRegistry().find("failCommand");
    failCreateIndexes
        ->setMode(FailPoint::alwaysOn,
                  0,
                  BSON("failCommands"
                       << BSON_ARRAY("createIndexes") << "namespace"
                       << NamespaceString::kConfigSampledQueriesDiffNamespace.toStringForErrorMsg()
                       << "errorCode" << ErrorCodes::IndexAlreadyExists << "failInternalCommands"
                       << true << "failLocalClients" << true));
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto future = writer.createTTLIndexes(operationContext());
    future.get();
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesNamespace,
                         QueryAnalysisWriter::kSampledQueriesTTLIndexName);
    assertTTLIndexExists(NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace,
                         QueryAnalysisWriter::kAnalyzeShardKeySplitPointsTTLIndexName);
}

TEST_F(QueryAnalysisWriterTest, CreateTTLIndexesWhenAnalyzeShardKeySplitPointsIndexExists) {
    auto failCreateIndexes = globalFailPointRegistry().find("failCommand");
    failCreateIndexes->setMode(
        FailPoint::alwaysOn,
        0,
        BSON("failCommands"
             << BSON_ARRAY("createIndexes") << "namespace"
             << NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace.toStringForErrorMsg()
             << "errorCode" << ErrorCodes::IndexAlreadyExists << "failInternalCommands" << true
             << "failLocalClients" << true));
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto future = writer.createTTLIndexes(operationContext());
    future.get();
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesNamespace,
                         QueryAnalysisWriter::kSampledQueriesTTLIndexName);
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesDiffNamespace,
                         QueryAnalysisWriter::kSampledQueriesDiffTTLIndexName);
}

TEST_F(QueryAnalysisWriterTest, CreateTTLIndexesWhenAllIndexesExist) {
    auto failCreateIndexes = globalFailPointRegistry().find("failCommand");
    failCreateIndexes->setMode(FailPoint::alwaysOn,
                               0,
                               BSON("failCommands" << BSON_ARRAY("createIndexes") << "errorCode"
                                                   << ErrorCodes::IndexAlreadyExists
                                                   << "failInternalCommands" << true
                                                   << "failLocalClients" << true));
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto future = writer.createTTLIndexes(operationContext());
    future.get();
}

TEST_F(QueryAnalysisWriterTest, CreateTTLIndexesRetriesOnIntermittentError) {
    auto failCreateIndexes = globalFailPointRegistry().find("failCommand");
    failCreateIndexes->setMode(FailPoint::nTimes,
                               5,
                               BSON("failCommands" << BSON_ARRAY("createIndexes") << "errorCode"
                                                   << ErrorCodes::NetworkTimeout
                                                   << "failInternalCommands" << true
                                                   << "failLocalClients" << true));
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto future = writer.createTTLIndexes(operationContext());
    future.get();
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesNamespace,
                         QueryAnalysisWriter::kSampledQueriesTTLIndexName);
    assertTTLIndexExists(NamespaceString::kConfigSampledQueriesDiffNamespace,
                         QueryAnalysisWriter::kSampledQueriesDiffTTLIndexName);
    assertTTLIndexExists(NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace,
                         QueryAnalysisWriter::kAnalyzeShardKeySplitPointsTTLIndexName);
}

TEST_F(QueryAnalysisWriterTest, CreateTTLIndexesStopsOnStepDown) {
    // Make a random createIndexes command fail with a PrimarySteppedDown error.
    static StaticImmortal<synchronized_value<PseudoRandom>> random{
        PseudoRandom{SecureRandom().nextInt64()}};
    auto numSkips = (*random)->nextInt64(QueryAnalysisWriter::kTTLIndexes.size());

    auto failCreateIndexes = globalFailPointRegistry().find("failCommand");
    failCreateIndexes->setMode(FailPoint::skip,
                               numSkips,
                               BSON("failCommands" << BSON_ARRAY("createIndexes") << "errorCode"
                                                   << ErrorCodes::PrimarySteppedDown
                                                   << "failInternalCommands" << true
                                                   << "failLocalClients" << true));
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto future = writer.createTTLIndexes(operationContext());
    ASSERT_THROWS_CODE(future.get(), AssertionException, ErrorCodes::PrimarySteppedDown);
    failCreateIndexes->setMode(FailPoint::off, 0);
}

TEST_F(QueryAnalysisWriterTest, NoQueries) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    writer.flushQueriesForTest(operationContext());
}

TEST_F(QueryAnalysisWriterTest, FindQuery) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto testFindCmdCommon = [&](const BSONObj& filter,
                                 const BSONObj& collation,
                                 int expirationSecs,
                                 const boost::optional<BSONObj>& letParameters = boost::none) {
        RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                        expirationSecs};
        auto sampleId = UUID::gen();

        writer.addFindQuery(sampleId, nss0, filter, collation, letParameters).get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);
        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);

        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
        assertSampledReadQueryDocument(sampleId,
                                       nss0,
                                       SampledCommandNameEnum::kFind,
                                       filter,
                                       collation,
                                       letParameters,
                                       expirationSecs);

        deleteSampledQueryDocuments();
    };

    testFindCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation(), oneSecondExpirationSecs);
    testFindCmdCommon(makeNonEmptyFilter(), emptyCollation, oneWeekExpirationSecs);
    testFindCmdCommon(emptyFilter, makeNonEmptyCollation(), oneMonthExpirationSecs);
    testFindCmdCommon(emptyFilter, emptyCollation, oneYearExpirationSecs);
    testFindCmdCommon(emptyFilter, emptyCollation, oneSecondExpirationSecs, makeLetParameters());
}

TEST_F(QueryAnalysisWriterTest, CountQuery) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto testCountCmdCommon =
        [&](const BSONObj& filter, const BSONObj& collation, int expirationSecs) {
            RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                            expirationSecs};

            auto sampleId = UUID::gen();

            writer.addCountQuery(sampleId, nss0, filter, collation).get();
            ASSERT_EQ(writer.getQueriesCountForTest(), 1);
            writer.flushQueriesForTest(operationContext());
            ASSERT_EQ(writer.getQueriesCountForTest(), 0);

            ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
            assertSampledReadQueryDocument(sampleId,
                                           nss0,
                                           SampledCommandNameEnum::kCount,
                                           filter,
                                           collation,
                                           boost::none /* letParameters */,
                                           expirationSecs);

            deleteSampledQueryDocuments();
        };

    testCountCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation(), oneSecondExpirationSecs);
    testCountCmdCommon(makeNonEmptyFilter(), emptyCollation, oneWeekExpirationSecs);
    testCountCmdCommon(emptyFilter, makeNonEmptyCollation(), oneMonthExpirationSecs);
    testCountCmdCommon(emptyFilter, emptyCollation, oneYearExpirationSecs);
}

TEST_F(QueryAnalysisWriterTest, DistinctQuery) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto testDistinctCmdCommon =
        [&](const BSONObj& filter, const BSONObj& collation, int expirationSecs) {
            RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                            expirationSecs};
            auto sampleId = UUID::gen();

            writer.addDistinctQuery(sampleId, nss0, filter, collation).get();
            ASSERT_EQ(writer.getQueriesCountForTest(), 1);
            writer.flushQueriesForTest(operationContext());
            ASSERT_EQ(writer.getQueriesCountForTest(), 0);

            ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
            assertSampledReadQueryDocument(sampleId,
                                           nss0,
                                           SampledCommandNameEnum::kDistinct,
                                           filter,
                                           collation,
                                           boost::none /* letParameters */,
                                           expirationSecs);

            deleteSampledQueryDocuments();
        };

    testDistinctCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation(), oneSecondExpirationSecs);
    testDistinctCmdCommon(makeNonEmptyFilter(), emptyCollation, oneWeekExpirationSecs);
    testDistinctCmdCommon(emptyFilter, makeNonEmptyCollation(), oneMonthExpirationSecs);
    testDistinctCmdCommon(emptyFilter, emptyCollation, oneYearExpirationSecs);
}

TEST_F(QueryAnalysisWriterTest, AggregateQuery) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto testAggregateCmdCommon = [&](const BSONObj& filter,
                                      const BSONObj& collation,
                                      int expirationSecs,
                                      const boost::optional<BSONObj>& letParameters = boost::none) {
        RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                        expirationSecs};
        auto sampleId = UUID::gen();

        writer.addAggregateQuery(sampleId, nss0, filter, collation, letParameters).get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);
        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);

        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
        assertSampledReadQueryDocument(sampleId,
                                       nss0,
                                       SampledCommandNameEnum::kAggregate,
                                       filter,
                                       collation,
                                       letParameters,
                                       expirationSecs);

        deleteSampledQueryDocuments();
    };

    testAggregateCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation(), oneSecondExpirationSecs);
    testAggregateCmdCommon(makeNonEmptyFilter(), emptyCollation, oneWeekExpirationSecs);
    testAggregateCmdCommon(emptyFilter, makeNonEmptyCollation(), oneMonthExpirationSecs);
    testAggregateCmdCommon(emptyFilter, emptyCollation, oneYearExpirationSecs);
    testAggregateCmdCommon(
        emptyFilter, emptyCollation, oneSecondExpirationSecs, makeLetParameters());
}

DEATH_TEST_F(QueryAnalysisWriterTest, UpdateQueryNotMarkedForSampling, "invariant") {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto [originalCmd, _] = makeUpdateCommandRequest(nss0, 1, {} /* markForSampling */);
    writer.addUpdateQuery(operationContext(), originalCmd, 0).get();
}

TEST_F(QueryAnalysisWriterTest, UpdateQueriesMarkedForSampling) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto [originalCmd, expectedSampledCmds] =
        makeUpdateCommandRequest(nss0, 3, {0, 2} /* markForSampling */);
    ASSERT_EQ(expectedSampledCmds.size(), 2U);

    auto expirationSecs = oneYearExpirationSecs;
    RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                    expirationSecs};

    writer.addUpdateQuery(operationContext(), originalCmd, 0).get();
    writer.addUpdateQuery(operationContext(), originalCmd, 2).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 2);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 2);
    for (const auto& [sampleId, expectedSampledCmd] : expectedSampledCmds) {
        assertSampledWriteQueryDocument(sampleId,
                                        expectedSampledCmd.getNamespace(),
                                        SampledCommandNameEnum::kUpdate,
                                        expectedSampledCmd,
                                        expirationSecs);
    }
}

DEATH_TEST_F(QueryAnalysisWriterTest, DeleteQueryNotMarkedForSampling, "invariant") {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto [originalCmd, _] = makeDeleteCommandRequest(nss0, 1, {} /* markForSampling */);
    writer.addDeleteQuery(operationContext(), originalCmd, 0).get();
}

TEST_F(QueryAnalysisWriterTest, DeleteQueriesMarkedForSampling) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto [originalCmd, expectedSampledCmds] =
        makeDeleteCommandRequest(nss0, 3, {1, 2} /* markForSampling */);
    ASSERT_EQ(expectedSampledCmds.size(), 2U);

    auto expirationSecs = oneYearExpirationSecs;
    RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                    expirationSecs};

    writer.addDeleteQuery(operationContext(), originalCmd, 1).get();
    writer.addDeleteQuery(operationContext(), originalCmd, 2).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 2);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 2);
    for (const auto& [sampleId, expectedSampledCmd] : expectedSampledCmds) {
        assertSampledWriteQueryDocument(sampleId,
                                        expectedSampledCmd.getNamespace(),
                                        SampledCommandNameEnum::kDelete,
                                        expectedSampledCmd,
                                        expirationSecs);
    }
}

DEATH_TEST_F(QueryAnalysisWriterTest, FindAndModifyQueryNotMarkedForSampling, "invariant") {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto [originalCmd, _] =
        makeFindAndModifyCommandRequest(nss0, true /* isUpdate */, false /* markForSampling */);
    writer.addFindAndModifyQuery(operationContext(), originalCmd).get();
}

TEST_F(QueryAnalysisWriterTest, FindAndModifyQueryUpdateMarkedForSampling) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto [originalCmd, expectedSampledCmds] =
        makeFindAndModifyCommandRequest(nss0, true /* isUpdate */, true /* markForSampling */);
    ASSERT_EQ(expectedSampledCmds.size(), 1U);
    auto [sampleId, expectedSampledCmd] = *expectedSampledCmds.begin();

    auto expirationSecs = oneYearExpirationSecs;
    RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                    expirationSecs};

    writer.addFindAndModifyQuery(operationContext(), originalCmd).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledWriteQueryDocument(sampleId,
                                    expectedSampledCmd.getNamespace(),
                                    SampledCommandNameEnum::kFindAndModify,
                                    expectedSampledCmd,
                                    expirationSecs);
}

TEST_F(QueryAnalysisWriterTest, FindAndModifyQueryRemoveMarkedForSampling) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto [originalCmd, expectedSampledCmds] =
        makeFindAndModifyCommandRequest(nss0, false /* isUpdate */, true /* markForSampling */);
    ASSERT_EQ(expectedSampledCmds.size(), 1U);
    auto [sampleId, expectedSampledCmd] = *expectedSampledCmds.begin();

    auto expirationSecs = oneYearExpirationSecs;
    RAIIServerParameterControllerForTest expiration{"queryAnalysisSampleExpirationSecs",
                                                    expirationSecs};

    writer.addFindAndModifyQuery(operationContext(), originalCmd).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledWriteQueryDocument(sampleId,
                                    expectedSampledCmd.getNamespace(),
                                    SampledCommandNameEnum::kFindAndModify,
                                    expectedSampledCmd,
                                    expirationSecs);
}

TEST_F(QueryAnalysisWriterTest, MultipleQueriesAndCollections) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    // Make nss0 have one query.
    auto [originalDeleteCmd, expectedSampledDeleteCmds] =
        makeDeleteCommandRequest(nss1, 3, {1} /* markForSampling */);
    ASSERT_EQ(expectedSampledDeleteCmds.size(), 1U);
    auto [deleteSampleId, expectedSampledDeleteCmd] = *expectedSampledDeleteCmds.begin();

    // Make nss1 have two queries.
    auto [originalUpdateCmd, expectedSampledUpdateCmds] =
        makeUpdateCommandRequest(nss0, 1, {0} /* markForSampling */);
    ASSERT_EQ(expectedSampledUpdateCmds.size(), 1U);
    auto [updateSampleId, expectedSampledUpdateCmd] = *expectedSampledUpdateCmds.begin();

    auto countSampleId = UUID::gen();
    auto originalCountFilter = makeNonEmptyFilter();
    auto originalCountCollation = makeNonEmptyCollation();

    writer.addDeleteQuery(operationContext(), originalDeleteCmd, 1).get();
    writer.addUpdateQuery(operationContext(), originalUpdateCmd, 0).get();
    writer.addCountQuery(countSampleId, nss1, originalCountFilter, originalCountCollation).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 3);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledWriteQueryDocument(deleteSampleId,
                                    expectedSampledDeleteCmd.getNamespace(),
                                    SampledCommandNameEnum::kDelete,
                                    expectedSampledDeleteCmd);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), 2);
    assertSampledWriteQueryDocument(updateSampleId,
                                    expectedSampledUpdateCmd.getNamespace(),
                                    SampledCommandNameEnum::kUpdate,
                                    expectedSampledUpdateCmd);
    assertSampledReadQueryDocument(countSampleId,
                                   nss1,
                                   SampledCommandNameEnum::kCount,
                                   originalCountFilter,
                                   originalCountCollation);
}

// Test that the QueryAnalysisWriter correctly discards a duplicate query.
TEST_F(QueryAnalysisWriterTest, RemoveDuplicateQueriesBasic) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto findSampleId = UUID::gen();
    auto originalFindFilter = makeNonEmptyFilter();
    auto originalFindCollation = makeNonEmptyCollation();

    auto [originalUpdateCmd, expectedSampledUpdateCmds] =
        makeUpdateCommandRequest(nss0, 1, {0} /* markForSampling */);
    ASSERT_EQ(expectedSampledUpdateCmds.size(), 1U);
    auto [updateSampleId, expectedSampledUpdateCmd] = *expectedSampledUpdateCmds.begin();

    auto countSampleId = UUID::gen();
    auto originalCountFilter = makeNonEmptyFilter();
    auto originalCountCollation = makeNonEmptyCollation();

    writer
        .addFindQuery(findSampleId,
                      nss0,
                      originalFindFilter,
                      originalFindCollation,
                      boost::none /* letParameters */)
        .get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledReadQueryDocument(findSampleId,
                                   nss0,
                                   SampledCommandNameEnum::kFind,
                                   originalFindFilter,
                                   originalFindCollation);

    writer.addUpdateQuery(operationContext(), originalUpdateCmd, 0).get();
    writer
        .addFindQuery(findSampleId,
                      nss0,
                      originalFindFilter,
                      originalFindCollation,
                      boost::none /* letParameters */)
        .get();  // This is a duplicate.
    writer.addCountQuery(countSampleId, nss0, originalCountFilter, originalCountCollation).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 3);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 3);
    assertSampledWriteQueryDocument(updateSampleId,
                                    expectedSampledUpdateCmd.getNamespace(),
                                    SampledCommandNameEnum::kUpdate,
                                    expectedSampledUpdateCmd);
    assertSampledReadQueryDocument(findSampleId,
                                   nss0,
                                   SampledCommandNameEnum::kFind,
                                   originalFindFilter,
                                   originalFindCollation);
    assertSampledReadQueryDocument(countSampleId,
                                   nss0,
                                   SampledCommandNameEnum::kCount,
                                   originalCountFilter,
                                   originalCountCollation);
}

// Test that the QueryAnalysisWriter correctly discards a duplicate query even when there is some
// other error in the same or subsequent insert batch.
TEST_F(QueryAnalysisWriterTest, RemoveDuplicateQueriesAfterOtherWriteError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    for (auto batchSize : {2, 3, 5}) {
        LOGV2(9881701,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "batchSize"_attr = batchSize);

        RAIIServerParameterControllerForTest maxBatchSize{"queryAnalysisWriterMaxBatchSize",
                                                          batchSize};

        auto sampleId0 = UUID::gen();
        auto filter0 = makeNonEmptyFilter();
        auto collation0 = makeNonEmptyCollation();

        writer.addFindQuery(sampleId0, nss0, filter0, collation0, boost::none /* letParameters */)
            .get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);

        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);
        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
        assertSampledReadQueryDocument(
            sampleId0, nss0, SampledCommandNameEnum::kFind, filter0, collation0);

        auto numQueries = 7;
        std::vector<SampledReadQuery> expectedSampledCmds;
        expectedSampledCmds.push_back({sampleId0, filter0, collation0});
        for (auto i = 1; i < numQueries; i++) {
            auto sampleId = UUID::gen();
            auto filter = makeNonEmptyFilter();
            auto collation = makeNonEmptyCollation();

            writer.addFindQuery(sampleId, nss0, filter, collation, boost::none /* letParameters */)
                .get();
            if (i == 2) {
                // This is a duplicate.
                writer
                    .addFindQuery(
                        sampleId0, nss0, filter0, collation0, boost::none /* letParameters */)
                    .get();
            }
            expectedSampledCmds.push_back({sampleId, filter, collation});
        }
        ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);

        // Set a failpoint to mock an error for the insert statement for query2 and then verify that
        // the duplicate query0 did not get added back. Please note that the queries in the buffer
        // are flushed from the back.
        // - When the batch size is 2, query0 is in the same insert batch as query2 (3rd batch).
        // - When the batch size is 3, query0 is also in the same insert batch as query2 (2nd
        //   batch).
        // - When the batch size is 5, query0 is in the insert batch before the one that query2 is
        //   in (1st batch).
        auto failingSampleId = expectedSampledCmds[2].sampleId;
        // The index of the insert statement for query2 depends on the batch size but it is not used
        // so just set it to a placeholder integer.
        auto failingIndex = 1;
        auto failpoint = configureMockErrorInsertResponseFailPoint(failingSampleId, failingIndex);

        writer.flushQueriesForTest(operationContext());
        auto docs = writer.getQueriesForTest();
        ASSERT_GT(docs.size(), 0);
        for (const auto& doc : docs) {
            auto queryDoc = SampledQueryDocument::parse(
                IDLParserContext("RemoveDuplicateQueriesOtherWriteError"), doc);
            ASSERT_NE(queryDoc.getSampleId(), sampleId0);
        }

        failpoint->setMode(FailPoint::Mode::off);
        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);
        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
        for (const auto& [sampleId, filter, collation] : expectedSampledCmds) {
            assertSampledReadQueryDocument(
                sampleId, nss0, SampledCommandNameEnum::kFind, filter, collation);
        }

        deleteSampledQueryDocuments();
    }
}

TEST_F(QueryAnalysisWriterTest, RemoveBadQueriesTopLevelError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    for (const auto& errorCode : QueryAnalysisWriter::kNonRetryableInsertErrorCodes) {
        if (errorCode == ErrorCodes::DuplicateKey) {
            // The DuplicateKey error is a write error and is tested separately in other unit tests.
            continue;
        }

        LOGV2(9885201,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "errorCode"_attr = errorCode);

        auto sampleId0 = UUID::gen();
        auto filter0 = makeNonEmptyFilter();
        auto collation0 = makeNonEmptyCollation();

        writer.addFindQuery(sampleId0, nss0, filter0, collation0, boost::none /* letParameters */)
            .get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);

        auto failpoint = globalFailPointRegistry().find("failCommand");
        failpoint->setMode(FailPoint::alwaysOn,
                           0,
                           BSON("failCommands" << BSON_ARRAY("insert") << "namespace"
                                               << NamespaceString::kConfigSampledQueriesNamespace
                                                      .toStringForErrorMsg()
                                               << "errorCode" << errorCode << "failInternalCommands"
                                               << true << "failLocalClients" << true));

        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);
        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 0);

        failpoint->setMode(FailPoint::off);
    }
}

TEST_F(QueryAnalysisWriterTest, RemoveBadQueriesWriteError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    for (const auto& errorCode : QueryAnalysisWriter::kNonRetryableInsertErrorCodes) {
        if (errorCode == ErrorCodes::DuplicateKey) {
            // The DuplicateKey error is a write error and is tested separately in other unit tests.
            continue;
        }

        LOGV2(9885202,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "errorCode"_attr = errorCode);

        auto sampleId0 = UUID::gen();
        auto filter0 = makeNonEmptyFilter();
        auto collation0 = makeNonEmptyCollation();

        writer.addFindQuery(sampleId0, nss0, filter0, collation0, boost::none /* letParameters */)
            .get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);

        auto failpoint =
            globalFailPointRegistry().find("queryAnalysisWriterMockInsertCommandResponse");
        failpoint->setMode(FailPoint::Mode::alwaysOn,
                           0,
                           BSON("_id" << sampleId0 << "errorDetails"
                                      << BSON("index" << 0 << "code" << errorCode << "errmsg"
                                                      << "Mock error")));

        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);

        failpoint->setMode(FailPoint::off);
    }
}

TEST_F(QueryAnalysisWriterTest, QueriesMultipleBatches_MaxBatchSize) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    RAIIServerParameterControllerForTest maxBatchSize{"queryAnalysisWriterMaxBatchSize", 2};
    auto numQueries = 5;

    std::vector<SampledReadQuery> expectedSampledCmds;
    for (auto i = 0; i < numQueries; i++) {
        auto sampleId = UUID::gen();
        auto filter = makeNonEmptyFilter();
        auto collation = makeNonEmptyCollation();

        writer.addAggregateQuery(sampleId, nss0, filter, collation, boost::none /* letParameters */)
            .get();
        expectedSampledCmds.push_back({sampleId, filter, collation});
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
    for (const auto& [sampleId, filter, collation] : expectedSampledCmds) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledCommandNameEnum::kAggregate, filter, collation);
    }
}

TEST_F(QueryAnalysisWriterTest, QueriesMultipleBatchesFewQueries_MaxBSONObjSize) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto numQueries = 3;
    std::vector<SampledReadQuery> expectedSampledCmds;
    for (auto i = 0; i < numQueries; i++) {
        auto sampleId = UUID::gen();
        auto filter = BSON(std::string(BSONObjMaxUserSize / 2, 'a') << 1);
        auto collation = makeNonEmptyCollation();

        writer.addAggregateQuery(sampleId, nss0, filter, collation, boost::none /* letParameters */)
            .get();
        expectedSampledCmds.push_back({sampleId, filter, collation});
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
    for (const auto& [sampleId, filter, collation] : expectedSampledCmds) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledCommandNameEnum::kAggregate, filter, collation);
    }
}

TEST_F(QueryAnalysisWriterTest, QueriesMultipleBatchesManyQueries_MaxBSONObjSize) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto numQueries = 75'000;
    std::vector<SampledReadQuery> expectedSampledCmds;
    for (auto i = 0; i < numQueries; i++) {
        auto sampleId = UUID::gen();
        auto filter = makeNonEmptyFilter();
        auto collation = makeNonEmptyCollation();

        writer.addAggregateQuery(sampleId, nss0, filter, collation, boost::none /* letParameters */)
            .get();
        expectedSampledCmds.push_back({sampleId, filter, collation});
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
    for (const auto& [sampleId, filter, collation] : expectedSampledCmds) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledCommandNameEnum::kAggregate, filter, collation);
    }
}

TEST_F(QueryAnalysisWriterTest, FlushAfterAddReadIfExceedsSizeLimit) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto maxMemoryUsageBytes = 1024;
    RAIIServerParameterControllerForTest maxMemoryBytes{"queryAnalysisWriterMaxMemoryUsageBytes",
                                                        maxMemoryUsageBytes};

    auto sampleId0 = UUID::gen();
    auto filter0 = BSON(std::string(maxMemoryUsageBytes / 2, 'a') << 1);
    auto collation0 = makeNonEmptyCollation();

    auto sampleId1 = UUID::gen();
    auto filter1 = BSON(std::string(maxMemoryUsageBytes / 2, 'b') << 1);
    auto collation1 = makeNonEmptyCollation();

    writer.addFindQuery(sampleId0, nss0, filter0, collation0, boost::none /* letParameters */)
        .get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    // Adding the next query causes the size to exceed the limit.
    writer.addAggregateQuery(sampleId1, nss1, filter1, collation1, boost::none /* letParameters */)
        .get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledReadQueryDocument(
        sampleId0, nss0, SampledCommandNameEnum::kFind, filter0, collation0);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), 1);
    assertSampledReadQueryDocument(
        sampleId1, nss1, SampledCommandNameEnum::kAggregate, filter1, collation1);
}

TEST_F(QueryAnalysisWriterTest, FlushAfterAddUpdateIfExceedsSizeLimit) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto maxMemoryUsageBytes = 1024;
    RAIIServerParameterControllerForTest maxMemoryBytes{"queryAnalysisWriterMaxMemoryUsageBytes",
                                                        maxMemoryUsageBytes};
    auto [originalCmd, expectedSampledCmds] =
        makeUpdateCommandRequest(nss0,
                                 3,
                                 {0, 2} /* markForSampling */,
                                 std::string(maxMemoryUsageBytes / 2, 'a') /* filterFieldName */);
    ASSERT_EQ(expectedSampledCmds.size(), 2U);

    writer.addUpdateQuery(operationContext(), originalCmd, 0).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    // Adding the next query causes the size to exceed the limit.
    writer.addUpdateQuery(operationContext(), originalCmd, 2).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 2);
    for (const auto& [sampleId, expectedSampledCmd] : expectedSampledCmds) {
        assertSampledWriteQueryDocument(sampleId,
                                        expectedSampledCmd.getNamespace(),
                                        SampledCommandNameEnum::kUpdate,
                                        expectedSampledCmd);
    }
}

TEST_F(QueryAnalysisWriterTest, FlushAfterAddDeleteIfExceedsSizeLimit) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto maxMemoryUsageBytes = 1024;
    RAIIServerParameterControllerForTest maxMemoryBytes{"queryAnalysisWriterMaxMemoryUsageBytes",
                                                        maxMemoryUsageBytes};
    auto [originalCmd, expectedSampledCmds] =
        makeDeleteCommandRequest(nss0,
                                 3,
                                 {0, 1} /* markForSampling */,
                                 std::string(maxMemoryUsageBytes / 2, 'a') /* filterFieldName */);
    ASSERT_EQ(expectedSampledCmds.size(), 2U);

    writer.addDeleteQuery(operationContext(), originalCmd, 0).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    // Adding the next query causes the size to exceed the limit.
    writer.addDeleteQuery(operationContext(), originalCmd, 1).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 2);
    for (const auto& [sampleId, expectedSampledCmd] : expectedSampledCmds) {
        assertSampledWriteQueryDocument(sampleId,
                                        expectedSampledCmd.getNamespace(),
                                        SampledCommandNameEnum::kDelete,
                                        expectedSampledCmd);
    }
}

TEST_F(QueryAnalysisWriterTest, FlushAfterAddFindAndModifyIfExceedsSizeLimit) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto maxMemoryUsageBytes = 1024;
    RAIIServerParameterControllerForTest maxMemoryBytes{"queryAnalysisWriterMaxMemoryUsageBytes",
                                                        maxMemoryUsageBytes};

    auto [originalCmd0, expectedSampledCmds0] = makeFindAndModifyCommandRequest(
        nss0,
        true /* isUpdate */,
        true /* markForSampling */,
        std::string(maxMemoryUsageBytes / 2, 'a') /* filterFieldName */);
    ASSERT_EQ(expectedSampledCmds0.size(), 1U);
    auto [sampleId0, expectedSampledCmd0] = *expectedSampledCmds0.begin();

    auto [originalCmd1, expectedSampledCmds1] = makeFindAndModifyCommandRequest(
        nss1,
        false /* isUpdate */,
        true /* markForSampling */,
        std::string(maxMemoryUsageBytes / 2, 'b') /* filterFieldName */);
    ASSERT_EQ(expectedSampledCmds0.size(), 1U);
    auto [sampleId1, expectedSampledCmd1] = *expectedSampledCmds1.begin();

    writer.addFindAndModifyQuery(operationContext(), originalCmd0).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    // Adding the next query causes the size to exceed the limit.
    writer.addFindAndModifyQuery(operationContext(), originalCmd1).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledWriteQueryDocument(sampleId0,
                                    expectedSampledCmd0.getNamespace(),
                                    SampledCommandNameEnum::kFindAndModify,
                                    expectedSampledCmd0);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), 1);
    assertSampledWriteQueryDocument(sampleId1,
                                    expectedSampledCmd1.getNamespace(),
                                    SampledCommandNameEnum::kFindAndModify,
                                    expectedSampledCmd1);
}

struct QueryAnalysisWriterTestAfterWriteError : public QueryAnalysisWriterTest {
protected:
    // The unit tests in this fixture would hang if mock clock is used.
    QueryAnalysisWriterTestAfterWriteError() : QueryAnalysisWriterTest(false /* useMockClock */) {}
};

TEST_F(QueryAnalysisWriterTestAfterWriteError, AddQueriesBackAfterWriteError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto originalFilter = makeNonEmptyFilter();
    auto originalCollation = makeNonEmptyCollation();
    auto numQueries = 8;

    std::vector<UUID> sampleIds0;
    for (auto i = 0; i < numQueries; i++) {
        sampleIds0.push_back(UUID::gen());
        writer
            .addFindQuery(sampleIds0[i],
                          nss0,
                          originalFilter,
                          originalCollation,
                          boost::none /* letParameters */)
            .get();
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);

    // Force the documents to get inserted in three batches of size 3, 3 and 2, respectively.
    RAIIServerParameterControllerForTest maxBatchSize{"queryAnalysisWriterMaxBatchSize", 3};

    // Hang after inserting the documents in the first batch.
    auto hangFp = globalFailPointRegistry().find("hangAfterCollectionInserts");
    auto hangTimesEntered = hangFp->setMode(FailPoint::alwaysOn, 0);

    auto future = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = makeOperationContext();
        writer.flushQueriesForTest(opCtx.get());
    });

    hangFp->waitForTimesEntered(hangTimesEntered + 1);
    // Force the second batch to fail so that it falls back to inserting one document at a time in
    // order, and then force the first and second document in the batch to fail.
    auto failFp = globalFailPointRegistry().find("failCollectionInserts");
    failFp->setMode(FailPoint::nTimes, 3);
    hangFp->setMode(FailPoint::off, 0);

    future.get();
    // Verify that all the documents other than the ones in the first batch got added back to the
    // buffer after the error. That is, the error caused the last document in the second batch to
    // get added to buffer also although it was successfully inserted since the writer did not have
    // a way to tell if the error caused the entire command to fail early.
    ASSERT_EQ(writer.getQueriesCountForTest(), 5);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 4);

    // Flush that remaining documents. If the documents were not added back correctly, some
    // documents would be missing and the checks below would fail.
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
    for (const auto& sampleId : sampleIds0) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledCommandNameEnum::kFind, originalFilter, originalCollation);
    }
}

TEST_F(QueryAnalysisWriterTestAfterWriteError, RemoveDuplicatesFromBufferAfterWriteError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto originalFilter = makeNonEmptyFilter();
    auto originalCollation = makeNonEmptyCollation();

    auto numQueries0 = 3;

    std::vector<UUID> sampleIds0;
    for (auto i = 0; i < numQueries0; i++) {
        sampleIds0.push_back(UUID::gen());
        writer
            .addFindQuery(sampleIds0[i],
                          nss0,
                          originalFilter,
                          originalCollation,
                          boost::none /* letParameters */)
            .get();
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries0);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries0);
    for (const auto& sampleId : sampleIds0) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledCommandNameEnum::kFind, originalFilter, originalCollation);
    }

    auto numQueries1 = 5;

    std::vector<UUID> sampleIds1;
    for (auto i = 0; i < numQueries1; i++) {
        sampleIds1.push_back(UUID::gen());
        writer
            .addFindQuery(sampleIds1[i],
                          nss1,
                          originalFilter,
                          originalCollation,
                          boost::none /* letParameters */)
            .get();
        // This is a duplicate.
        if (i < numQueries0) {
            writer
                .addFindQuery(sampleIds0[i],
                              nss0,
                              originalFilter,
                              originalCollation,
                              boost::none /* letParameters */)
                .get();
        }
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries0 + numQueries1);

    // Force the batch to fail so that it falls back to inserting one document at a time in order.
    auto failFp = globalFailPointRegistry().find("failCollectionInserts");
    failFp->setMode(FailPoint::nTimes, 1);

    // Hang after inserting the first non-duplicate document.
    auto hangFp = globalFailPointRegistry().find("hangAfterCollectionInserts");
    auto hangTimesEntered = hangFp->setMode(FailPoint::alwaysOn, 0);

    auto future = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = makeOperationContext();
        writer.flushQueriesForTest(opCtx.get());
    });

    hangFp->waitForTimesEntered(hangTimesEntered + 1);
    // Force the next non-duplicate document to fail to insert.
    failFp->setMode(FailPoint::nTimes, 1);
    hangFp->setMode(FailPoint::off, 0);

    future.get();
    // Verify that the duplicate documents did not get added back to the buffer after the error.
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries1);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), numQueries1 - 1);

    // Flush that remaining documents. If the documents were not added back correctly, the document
    // that previously failed to insert would be missing and the checks below would fail.
    failFp->setMode(FailPoint::off, 0);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), numQueries1);
    for (const auto& sampleId : sampleIds1) {
        assertSampledReadQueryDocument(
            sampleId, nss1, SampledCommandNameEnum::kFind, originalFilter, originalCollation);
    }
}

TEST_F(QueryAnalysisWriterTest, NoDiffs) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    writer.flushQueriesForTest(operationContext());
}

TEST_F(QueryAnalysisWriterTest, DiffsBasic) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto collUuid0 = getCollectionUUID(nss0);
    auto sampleId = UUID::gen();
    auto preImage = BSON("a" << 0);
    auto postImage = BSON("a" << 1);

    writer.addDiff(sampleId, nss0, collUuid0, preImage, postImage).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 1);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), 1);
    assertDiffDocument(sampleId, nss0, *doc_diff::computeInlineDiff(preImage, postImage));
}

TEST_F(QueryAnalysisWriterTest, DiffsMultipleQueriesAndCollections) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    // Make nss0 have a diff for one query.
    auto collUuid0 = getCollectionUUID(nss0);

    auto sampleId0 = UUID::gen();
    auto preImage0 = BSON("a" << 0 << "b" << 0 << "c" << 0);
    auto postImage0 = BSON("a" << 0 << "b" << 1 << "d" << 1);

    // Make nss1 have diffs for two queries.
    auto collUuid1 = getCollectionUUID(nss1);

    auto sampleId1 = UUID::gen();
    auto preImage1 = BSON("a" << 1 << "b" << BSON_ARRAY(1) << "d" << BSON("e" << 1));
    auto postImage1 = BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2) << "d" << BSON("e" << 2));

    auto sampleId2 = UUID::gen();
    auto preImage2 = BSON("a" << BSONObj());
    auto postImage2 = BSON("a" << BSON("b" << 2));

    writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0).get();
    writer.addDiff(sampleId1, nss1, collUuid1, preImage1, postImage1).get();
    writer.addDiff(sampleId2, nss1, collUuid1, preImage2, postImage2).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 3);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), 1);
    assertDiffDocument(sampleId0, nss0, *doc_diff::computeInlineDiff(preImage0, postImage0));

    ASSERT_EQ(getDiffDocumentsCount(nss1), 2);
    assertDiffDocument(sampleId1, nss1, *doc_diff::computeInlineDiff(preImage1, postImage1));
    assertDiffDocument(sampleId2, nss1, *doc_diff::computeInlineDiff(preImage2, postImage2));
}

// Test that the QueryAnalysisWriter correctly discards a duplicate diff.
TEST_F(QueryAnalysisWriterTest, RemoveDuplicateDiffsBasic) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto collUuid0 = getCollectionUUID(nss0);

    auto sampleId0 = UUID::gen();
    auto preImage0 = BSON("a" << 0);
    auto postImage0 = BSON("a" << 1);

    auto sampleId1 = UUID::gen();
    auto preImage1 = BSON("a" << 1 << "b" << BSON_ARRAY(1));
    auto postImage1 = BSON("a" << 1 << "b" << BSON_ARRAY(1 << 2));

    auto sampleId2 = UUID::gen();
    auto preImage2 = BSON("a" << BSONObj());
    auto postImage2 = BSON("a" << BSON("b" << 2));

    writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 1);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), 1);
    assertDiffDocument(sampleId0, nss0, *doc_diff::computeInlineDiff(preImage0, postImage0));

    writer.addDiff(sampleId1, nss0, collUuid0, preImage1, postImage1).get();
    writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0)
        .get();  // This is a duplicate.
    writer.addDiff(sampleId2, nss0, collUuid0, preImage2, postImage2).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 3);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), 3);
    assertDiffDocument(sampleId0, nss0, *doc_diff::computeInlineDiff(preImage0, postImage0));
    assertDiffDocument(sampleId1, nss0, *doc_diff::computeInlineDiff(preImage1, postImage1));
    assertDiffDocument(sampleId2, nss0, *doc_diff::computeInlineDiff(preImage2, postImage2));
}

// Test that the QueryAnalysisWriter correctly discards a duplicate diff even when there is some
// other error in the same or subsequent insert batch.
TEST_F(QueryAnalysisWriterTest, RemoveDuplicateDiffsAfterOtherWriteError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto collUuid0 = getCollectionUUID(nss0);

    for (auto batchSize : {2, 3, 5}) {
        LOGV2(9881702,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "batchSize"_attr = batchSize);

        RAIIServerParameterControllerForTest maxBatchSize{"queryAnalysisWriterMaxBatchSize",
                                                          batchSize};

        auto sampleId0 = UUID::gen();
        auto preImage0 = BSON("a0" << 0);
        auto postImage0 = BSON("a0" << 1);
        auto diff0 = *doc_diff::computeInlineDiff(preImage0, postImage0);

        writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0).get();
        ASSERT_EQ(writer.getDiffsCountForTest(), 1);

        writer.flushDiffsForTest(operationContext());
        ASSERT_EQ(writer.getDiffsCountForTest(), 0);
        ASSERT_EQ(getDiffDocumentsCount(nss0), 1);
        assertDiffDocument(sampleId0, nss0, diff0);

        auto numDiffs = 7;
        std::vector<SampledDiff> expectedSampledDiffs;
        expectedSampledDiffs.push_back({sampleId0, preImage0, postImage0, diff0});
        for (auto i = 1; i < numDiffs; i++) {
            auto sampleId = UUID::gen();
            auto fieldName = "a" + std::to_string(i);
            auto preImage = BSON(fieldName << 0);
            auto postImage = BSON(fieldName << 1);
            auto diff = *doc_diff::computeInlineDiff(preImage, postImage);

            writer.addDiff(sampleId, nss0, collUuid0, preImage, postImage).get();
            if (i == 2) {
                // This is a duplicate.
                writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0).get();
            }
            expectedSampledDiffs.push_back(
                {sampleId, std::move(preImage), std::move(postImage), std::move(diff)});
        }
        ASSERT_EQ(writer.getDiffsCountForTest(), numDiffs);

        // Set a failpoint to mock an error for the insert statement for diff2 and then verify that
        // the duplicate diff0 did not get added back. Please note that the diffs in the buffer are
        // flushed from the back.
        // - When the batch size is 2, diff0 is in the same insert batch as diff2 (3rd batch).
        // - When the batch size is 3, diff0 is also in the same insert batch as diff2 (2nd batch).
        // - When the batch size is 5, diff0 is in the insert batch before the one that diff2 is in
        //   (1st batch).
        auto failingSampleId = expectedSampledDiffs[2].sampleId;
        // The index of the insert statement for diff2 depends on the batch size but it is not used
        // so just set it to a placeholder integer.
        auto failingIndex = 1;
        auto failpoint = configureMockErrorInsertResponseFailPoint(failingSampleId, failingIndex);

        writer.flushDiffsForTest(operationContext());
        auto docs = writer.getDiffsForTest();
        ASSERT_GT(docs.size(), 0);
        for (const auto& doc : docs) {
            auto diffDoc = SampledQueryDiffDocument::parse(
                IDLParserContext("RemoveDuplicateDiffsAfterOtherWriteError"), doc);
            ASSERT_NE(diffDoc.getSampleId(), sampleId0);
        }

        failpoint->setMode(FailPoint::Mode::off);
        writer.flushDiffsForTest(operationContext());
        ASSERT_EQ(getDiffDocumentsCount(nss0), numDiffs);
        for (const auto& [sampleId, _, __, diff] : expectedSampledDiffs) {
            assertDiffDocument(sampleId, nss0, diff);
        }

        deleteSampledDiffDocuments();
    }
}

TEST_F(QueryAnalysisWriterTest, RemoveBadDiffsTopLevelError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto collUuid0 = getCollectionUUID(nss0);

    for (auto errorCode : QueryAnalysisWriter::kNonRetryableInsertErrorCodes) {
        if (errorCode == ErrorCodes::DuplicateKey) {
            // The DuplicateKey error is a write error and is tested separately in other unit tests.
            continue;
        }

        LOGV2(9881703,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "errorCode"_attr = errorCode);

        auto sampleId0 = UUID::gen();
        auto preImage0 = BSON("a0" << 0);
        auto postImage0 = BSON("a0" << 1);

        writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0).get();
        ASSERT_EQ(writer.getDiffsCountForTest(), 1);

        auto failpoint = globalFailPointRegistry().find("failCommand");
        failpoint->setMode(
            FailPoint::alwaysOn,
            0,
            BSON("failCommands"
                 << BSON_ARRAY("insert") << "namespace"
                 << NamespaceString::kConfigSampledQueriesDiffNamespace.toStringForErrorMsg()
                 << "errorCode" << errorCode << "failInternalCommands" << true << "failLocalClients"
                 << true));

        writer.flushDiffsForTest(operationContext());
        ASSERT_EQ(writer.getDiffsCountForTest(), 0);
        ASSERT_EQ(getDiffDocumentsCount(nss0), 0);

        failpoint->setMode(FailPoint::off);
    }
}

TEST_F(QueryAnalysisWriterTest, RemoveBadDiffsWriteError) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());
    auto collUuid0 = getCollectionUUID(nss0);

    for (const auto& errorCode : QueryAnalysisWriter::kNonRetryableInsertErrorCodes) {
        if (errorCode == ErrorCodes::DuplicateKey) {
            // The DuplicateKey error is a write error and is tested separately in other unit tests.
            continue;
        }

        LOGV2(9885204,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "errorCode"_attr = errorCode);

        auto sampleId0 = UUID::gen();
        auto preImage0 = BSON("a0" << 0);
        auto postImage0 = BSON("a0" << 1);

        writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0).get();
        ASSERT_EQ(writer.getDiffsCountForTest(), 1);

        auto failpoint =
            globalFailPointRegistry().find("queryAnalysisWriterMockInsertCommandResponse");
        failpoint->setMode(FailPoint::Mode::alwaysOn,
                           0,
                           BSON("_id" << sampleId0 << "errorDetails"
                                      << BSON("index" << 0 << "code" << errorCode << "errmsg"
                                                      << "Mock error")));

        writer.flushDiffsForTest(operationContext());
        ASSERT_EQ(writer.getDiffsCountForTest(), 0);

        failpoint->setMode(FailPoint::off);
    }
}

TEST_F(QueryAnalysisWriterTest, DiffsMultipleBatches_MaxBatchSize) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    RAIIServerParameterControllerForTest maxBatchSize{"queryAnalysisWriterMaxBatchSize", 2};
    auto numDiffs = 5;
    auto collUuid0 = getCollectionUUID(nss0);

    std::vector<SampledDiff> expectedSampledDiffs;
    for (auto i = 0; i < numDiffs; i++) {
        auto sampleId = UUID::gen();
        auto preImage = BSON("a" << 0);
        auto postImage = BSON(("a" + std::to_string(i)) << 1);
        auto diff = *doc_diff::computeInlineDiff(preImage, postImage);

        writer.addDiff(sampleId, nss0, collUuid0, preImage, postImage).get();
        expectedSampledDiffs.push_back({sampleId, preImage, postImage, diff});
    }
    ASSERT_EQ(writer.getDiffsCountForTest(), numDiffs);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), numDiffs);
    for (const auto& [sampleId, _, __, diff] : expectedSampledDiffs) {
        assertDiffDocument(sampleId, nss0, diff);
    }
}

TEST_F(QueryAnalysisWriterTest, DiffsMultipleBatches_MaxBSONObjSize) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto numDiffs = 3;
    auto collUuid0 = getCollectionUUID(nss0);

    std::vector<SampledDiff> expectedSampledDiffs;
    for (auto i = 0; i < numDiffs; i++) {
        auto sampleId = UUID::gen();
        auto preImage = BSON("a" << 0);
        auto postImage = BSON(std::string(BSONObjMaxUserSize / 2, 'a') << 1);
        auto diff = *doc_diff::computeInlineDiff(preImage, postImage);

        writer.addDiff(sampleId, nss0, collUuid0, preImage, postImage).get();
        expectedSampledDiffs.push_back({sampleId, preImage, postImage, diff});
    }
    ASSERT_EQ(writer.getDiffsCountForTest(), numDiffs);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), numDiffs);
    for (const auto& [sampleId, _, __, diff] : expectedSampledDiffs) {
        assertDiffDocument(sampleId, nss0, diff);
    }
}

TEST_F(QueryAnalysisWriterTest, FlushAfterAddDiffIfExceedsSizeLimit) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto maxMemoryUsageBytes = 1024;
    RAIIServerParameterControllerForTest maxMemoryBytes{"queryAnalysisWriterMaxMemoryUsageBytes",
                                                        maxMemoryUsageBytes};

    auto collUuid0 = getCollectionUUID(nss0);
    auto sampleId0 = UUID::gen();
    auto preImage0 = BSON("a" << 0);
    auto postImage0 = BSON(std::string(maxMemoryUsageBytes / 2, 'a') << 1);

    auto collUuid1 = getCollectionUUID(nss1);
    auto sampleId1 = UUID::gen();
    auto preImage1 = BSON("a" << 0);
    auto postImage1 = BSON(std::string(maxMemoryUsageBytes / 2, 'b') << 1);

    writer.addDiff(sampleId0, nss0, collUuid0, preImage0, postImage0).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 1);
    // Adding the next diff causes the size to exceed the limit.
    writer.addDiff(sampleId1, nss1, collUuid1, preImage1, postImage1).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), 1);
    assertDiffDocument(sampleId0, nss0, *doc_diff::computeInlineDiff(preImage0, postImage0));
    ASSERT_EQ(getDiffDocumentsCount(nss1), 1);
    assertDiffDocument(sampleId1, nss1, *doc_diff::computeInlineDiff(preImage1, postImage1));
}

TEST_F(QueryAnalysisWriterTest, DiffEmpty) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto collUuid0 = getCollectionUUID(nss0);
    auto sampleId = UUID::gen();
    auto preImage = BSON("a" << 1);
    auto postImage = preImage;

    writer.addDiff(sampleId, nss0, collUuid0, preImage, postImage).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), 0);
}

TEST_F(QueryAnalysisWriterTest, DiffExceedsSizeLimit) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    auto collUuid0 = getCollectionUUID(nss0);
    auto sampleId = UUID::gen();
    auto preImage = BSON(std::string(BSONObjMaxUserSize, 'a') << 1);
    auto postImage = BSONObj();

    writer.addDiff(sampleId, nss0, collUuid0, preImage, postImage).get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);
    writer.flushDiffsForTest(operationContext());
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);

    ASSERT_EQ(getDiffDocumentsCount(nss0), 0);
}

void QueryAnalysisWriterTest::assertNoSampling(const NamespaceString& nss, const UUID& collUuid) {
    auto& writer = *QueryAnalysisWriter::get(operationContext());

    writer
        .addFindQuery(UUID::gen() /* sampleId */,
                      nss,
                      emptyFilter,
                      emptyCollation,
                      boost::none /* letParameters */)
        .get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    writer
        .addAggregateQuery(UUID::gen() /* sampleId */,
                           nss,
                           emptyFilter,
                           emptyCollation,
                           boost::none /* letParameters */)
        .get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    writer.addCountQuery(UUID::gen() /* sampleId */, nss, emptyFilter, emptyCollation).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    writer.addDistinctQuery(UUID::gen() /* sampleId */, nss, emptyFilter, emptyCollation).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    auto originalUpdateCmd = makeUpdateCommandRequest(nss, 1, {0} /* markForSampling */).first;
    writer.addUpdateQuery(operationContext(), originalUpdateCmd, 0).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    auto originalDeleteCmd = makeDeleteCommandRequest(nss, 1, {0} /* markForSampling */).first;
    writer.addDeleteQuery(operationContext(), originalDeleteCmd, 0).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    auto originalFindAndModifyCmd =
        makeFindAndModifyCommandRequest(nss, true /* isUpdate */, true /* markForSampling */).first;
    writer.addFindAndModifyQuery(operationContext(), originalFindAndModifyCmd).get();

    writer
        .addDiff(UUID::gen() /* sampleId */,
                 nss,
                 collUuid,
                 BSON("a" << 0) /* preImage */,
                 BSON("a" << 1) /* postImage */)
        .get();
    ASSERT_EQ(writer.getDiffsCountForTest(), 0);
}

TEST_F(QueryAnalysisWriterTest, DiscardSamplesIfCollectionNoLongerExists) {
    DBDirectClient client(operationContext());
    auto collUuid0BeforeDrop = getCollectionUUID(nss0);
    client.dropCollection(nss0);
    assertNoSampling(nss0, collUuid0BeforeDrop);
}

TEST_F(QueryAnalysisWriterTest, DiscardSamplesIfCollectionIsDroppedAndRecreated) {
    DBDirectClient client(operationContext());
    auto collUuid0BeforeDrop = getCollectionUUID(nss0);
    client.dropCollection(nss0);
    client.createCollection(nss0);
    assertNoSampling(nss0, collUuid0BeforeDrop);
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
