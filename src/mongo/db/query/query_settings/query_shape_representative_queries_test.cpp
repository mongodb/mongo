/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_backfill.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/executor/mock_async_rpc.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

static auto const kNss = NamespaceString::createNamespaceString_forTest("foo", "exampleColl");
static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

class RepresentativeQueryTestFixture : public ShardingTestFixture {
public:
    void setUp() override {
        ShardingTestFixture::setUp();
        auto runner = std::make_unique<async_rpc::AsyncMockAsyncRPCRunner>();
        async_rpc::detail::AsyncRPCRunner::set(getServiceContext(), std::move(runner));
    }

    async_rpc::AsyncMockAsyncRPCRunner* runner() {
        return dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
            async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));
    }

    query_settings::QueryShapeRepresentativeQuery makeRepresentativeQuery(BSONObj filter) {
        // Create the find representative query.
        auto findCmd = query_request_helper::makeFromFindCommandForTests(
            [&] {
                BSONObjBuilder bb;
                bb.append("find"_sd, kNss.coll());
                bb.append("filter"_sd, filter);
                bb.append("$db"_sd, kNss.db(omitTenant));
                return bb.obj();
            }(),
            kNss);
        auto representativeQuery = findCmd->toBSON().getOwned();

        // Compute the query shape hash.
        auto* opCtx = operationContext();
        auto expCtx = makeBlankExpressionContext(opCtx, kNss);
        auto parsedFindCommand = uassertStatusOK(
            parsed_find_command::parse(expCtx, {.findCommand = std::move(findCmd)}));
        auto queryShapeHash =
            uassertStatusOK(
                shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedFindCommand, expCtx))
                ->sha256Hash(opCtx, kSerializationContext);
        return query_settings::QueryShapeRepresentativeQuery{std::move(queryShapeHash),
                                                             std::move(representativeQuery),
                                                             /* lastModifiedTime */ LogicalTime()};
    }

    async_rpc::AsyncMockAsyncRPCRunner::RequestMatcher makeMatcher(
        stdx::unordered_set<std::string> expectedHashes) {
        return [expectedHashes = std::move(expectedHashes)](
                   const async_rpc::AsyncMockAsyncRPCRunner::Request& req) {
            // Check if this is an insert command.
            const bool isInsert = req.cmdBSON.firstElementFieldNameStringData() == "insert"_sd;
            if (!isInsert) {
                return false;
            }

            // Check that the request is aimed at the representative queries collection.
            auto reqWithDb = req.cmdBSON.addFields(BSON("$db" << req.dbName));
            auto insertOp = write_ops::InsertCommandRequest::parse(
                reqWithDb, IDLParserContext("representativeQueryTestFixture"));
            if (insertOp.getNamespace() !=
                NamespaceString::kQueryShapeRepresentativeQueriesNamespace) {
                return false;
            }

            // Ensure that the insert contains all the hashes.
            auto&& documents = insertOp.getDocuments();
            for (auto&& doc : documents) {
                std::string insertedHash{doc.getStringField("_id"_sd)};
                if (!expectedHashes.count(insertedHash)) {
                    return false;
                }
            }
            return documents.size() == expectedHashes.size();
        };
    }
};

std::unique_ptr<async_rpc::Targeter> makeTargeter(OperationContext*) {
    return std::make_unique<async_rpc::LocalHostTargeter>();
}

TEST_F(RepresentativeQueryTestFixture, SimpleInsertSucceed) {
    auto representativeQuery = makeRepresentativeQuery(fromjson("{a: 1}"));
    auto&& hash = representativeQuery.get_id();
    auto response = [] {
        write_ops::InsertCommandReply reply;
        BSONObjBuilder bb(reply.toBSON());
        CommandHelpers::appendCommandStatusNoThrow(bb, Status::OK());
        return bb.obj();
    }();
    auto expectation = runner()->expect(
        makeMatcher({hash.toHexString()}), std::move(response), "mockInsertReponse");
    auto* opCtx = operationContext();
    auto insertedHashes = query_settings::insertRepresentativeQueriesToCollection(
                              opCtx, {representativeQuery}, makeTargeter, executor())
                              .get();
    ASSERT_EQ(1, insertedHashes.size());
    ASSERT_EQ(hash, insertedHashes[0]);
    expectation.get();
}

TEST_F(RepresentativeQueryTestFixture, DuplicateKeyErrorsSucceed) {
    auto representativeQuery0 = makeRepresentativeQuery(fromjson("{a: 1}"));
    auto representativeQuery1 = makeRepresentativeQuery(fromjson("{b: 1}"));
    auto response = [] {
        write_ops::InsertCommandReply reply;
        reply.setWriteErrors({{write_ops::WriteError(
            0,
            Status{DuplicateKeyErrorInfo{BSONObj(), BSONObj(), BSONObj(), BSONObj(), boost::none},
                   "Duplicate document found"})}});
        BSONObjBuilder bb(reply.toBSON());
        CommandHelpers::appendCommandStatusNoThrow(bb, Status::OK());
        return bb.obj();
    }();
    auto expectation = runner()->expect(makeMatcher({representativeQuery0.get_id().toHexString(),
                                                     representativeQuery1.get_id().toHexString()}),
                                        std::move(response),
                                        "mockDuplicateError");
    auto* opCtx = operationContext();
    auto insertedHashes =
        query_settings::insertRepresentativeQueriesToCollection(
            opCtx, {representativeQuery0, representativeQuery1}, makeTargeter, executor())
            .get();
    ASSERT_EQ(2, insertedHashes.size());
    expectation.get();
}

TEST_F(RepresentativeQueryTestFixture, NonDuplicateKeyWriteErrorsPartiallyFail) {
    auto representativeQuery0 = makeRepresentativeQuery(fromjson("{a: 1}"));
    auto representativeQuery1 = makeRepresentativeQuery(fromjson("{b: 1}"));
    auto response = [] {
        write_ops::InsertCommandReply reply;
        reply.setWriteErrors(
            {{write_ops::WriteError(0, Status{ErrorCodes::OutOfDiskSpace, "no disk"})}});
        BSONObjBuilder bb(reply.toBSON());
        CommandHelpers::appendCommandStatusNoThrow(bb, Status::OK());
        return bb.obj();
    }();
    auto expectation = runner()->expect(makeMatcher({representativeQuery0.get_id().toHexString(),
                                                     representativeQuery1.get_id().toHexString()}),
                                        std::move(response),
                                        "mockOutOfDiskSpaceError");
    auto* opCtx = operationContext();
    auto insertedHashes =
        query_settings::insertRepresentativeQueriesToCollection(
            opCtx, {representativeQuery0, representativeQuery1}, makeTargeter, executor())
            .get();
    ASSERT_EQ(1, insertedHashes.size());
    expectation.get();
}

TEST_F(RepresentativeQueryTestFixture, UnrecoverableErrorsCompletelyFail) {
    auto representativeQuery = makeRepresentativeQuery(fromjson("{a: 1}"));
    auto runTest = [&](Status status) {
        auto expectation =
            runner()->expect(makeMatcher({representativeQuery.get_id().toHexString()}),
                             status,
                             "mockNetworkErrorResponse");
        auto* opCtx = operationContext();
        auto insertedHashes = query_settings::insertRepresentativeQueriesToCollection(
                                  opCtx, {representativeQuery}, makeTargeter, executor())
                                  .get();
        ASSERT_EQ(0, insertedHashes.size());
        expectation.get();
    };
    runTest(Status{ErrorCodes::NetworkTimeout, "mockNetworkIssue"});
    runTest(Status{ErrorCodes::UnsatisfiableWriteConcern, "mockWriteConcernError"});
    runTest(Status{ErrorCodes::UnknownError, "mockUnknownError"});
}

TEST_F(RepresentativeQueryTestFixture, LargeBatchesAreChunked) {
    auto representativeQuery0 =
        makeRepresentativeQuery(BSON(std::string(BSONObjMaxUserSize, 'a') << true));
    auto representativeQuery1 =
        makeRepresentativeQuery(BSON(std::string(BSONObjMaxUserSize, 'b') << true));
    auto response = [] {
        write_ops::InsertCommandReply reply;
        BSONObjBuilder bb(reply.toBSON());
        CommandHelpers::appendCommandStatusNoThrow(bb, Status::OK());
        return bb.obj();
    }();
    std::vector<SemiFuture<void>> futures;
    futures.push_back(runner()->expect(
        makeMatcher({representativeQuery0.get_id().toHexString()}), response, "largeCommand0"));
    futures.push_back(runner()->expect(
        makeMatcher({representativeQuery1.get_id().toHexString()}), response, "largeCommand1"));
    auto* opCtx = operationContext();
    auto insertedHashes =
        query_settings::insertRepresentativeQueriesToCollection(
            opCtx, {representativeQuery0, representativeQuery1}, makeTargeter, executor())
            .get();
    ASSERT_EQ(2, insertedHashes.size());
    for (auto&& expectation : futures) {
        expectation.get();
    }
}

}  // namespace
}  // namespace mongo
