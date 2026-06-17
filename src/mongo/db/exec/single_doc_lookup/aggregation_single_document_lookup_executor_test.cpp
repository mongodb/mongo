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

#include "mongo/db/exec/single_doc_lookup/aggregation_single_document_lookup_executor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {
namespace {

using HandledStatus = SingleDocumentLookupExecutor::LookupResult::HandledStatus;

/**
 * Captures the arguments lookupSingleDocument() is called with (notably the readConcern the
 * executor builds) and returns a preconfigured result, so we can assert the executor's
 * readConcern construction and found / not-found mapping without a routing environment.
 */
class MockLookupSingleDocumentProcessInterface : public StubMongoProcessInterface {
public:
    boost::optional<Document> lookupSingleDocument(const boost::intrusive_ptr<ExpressionContext>&,
                                                   const NamespaceString& nss,
                                                   boost::optional<UUID> collectionUUID,
                                                   const Document& documentKey,
                                                   boost::optional<BSONObj> readConcern) override {
        ++callCount;
        lastNss = nss;
        lastCollectionUUID = collectionUUID;
        lastDocumentKey = documentKey;
        lastReadConcern = std::move(readConcern);
        return _result;
    }

    boost::optional<Document> _result;

    int callCount = 0;
    NamespaceString lastNss;
    boost::optional<UUID> lastCollectionUUID;
    Document lastDocumentKey;
    boost::optional<BSONObj> lastReadConcern;
};

class AggregationSingleDocumentLookupExecutorTest : public AggregationContextFixture {
protected:
    std::shared_ptr<MockLookupSingleDocumentProcessInterface> installMock(
        boost::optional<Document> result) {
        auto mock = std::make_shared<MockLookupSingleDocumentProcessInterface>();
        mock->_result = std::move(result);
        getExpCtx()->setMongoProcessInterface(mock);
        return mock;
    }

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");
    const UUID collectionUUID = UUID::gen();
    const Document documentKey = Document{{"_id", 7}};
};

TEST_F(AggregationSingleDocumentLookupExecutorTest, FoundDocumentReturnsKHandledFound) {
    auto mock = installMock(Document{{"_id", 7}, {"v", 1}});
    AggregationSingleDocumentLookupExecutor executor;

    auto result =
        executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));

    ASSERT(result.status == HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 7 << "v" << 1));
    ASSERT_EQ(mock->callCount, 1);
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, AbsentDocumentReturnsKHandledNotFound) {
    auto mock = installMock(boost::none);
    AggregationSingleDocumentLookupExecutor executor;

    auto result =
        executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));

    ASSERT(result.status == HandledStatus::kDocumentNotFound);
    ASSERT_FALSE(result.document.has_value());
    ASSERT_EQ(mock->callCount, 1);
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, BuildsMajorityReadConcernFromAfterClusterTime) {
    auto mock = installMock(boost::none);
    AggregationSingleDocumentLookupExecutor executor;

    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));

    ASSERT(mock->lastReadConcern.has_value());
    ASSERT_BSONOBJ_EQ(*mock->lastReadConcern,
                      BSON("level" << "majority"
                                   << "afterClusterTime" << Timestamp(100, 1)));
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, NoReadConcernWhenAfterClusterTimeAbsent) {
    auto mock = installMock(boost::none);
    AggregationSingleDocumentLookupExecutor executor;

    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, boost::none);

    ASSERT_FALSE(mock->lastReadConcern.has_value());
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, ForwardsNssUuidAndDocumentKey) {
    auto mock = installMock(boost::none);
    AggregationSingleDocumentLookupExecutor executor;

    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, boost::none);

    ASSERT_EQ(mock->lastNss, nss);
    ASSERT(mock->lastCollectionUUID == boost::optional<UUID>(collectionUUID));
    ASSERT_BSONOBJ_EQ(mock->lastDocumentKey.toBson(), documentKey.toBson());
}

}  // namespace
}  // namespace mongo::exec::agg
