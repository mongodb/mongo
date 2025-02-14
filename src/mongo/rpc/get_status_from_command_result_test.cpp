/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
Status statusFor(const std::string& json) {
    return getStatusFromCommandResult(fromjson(json));
}
}  // namespace

TEST(GetStatusFromCommandResult, Ok) {
    ASSERT_OK(statusFor("{ok: 1.0}"));
}

TEST(GetStatusFromCommandResult, OkIgnoresCode) {
    ASSERT_OK(statusFor("{ok: 1.0, code: 12345, errmsg: 'oh no!'}"));
}

TEST(GetStatusFromCommandResult, SimpleBad) {
    const auto status = statusFor("{ok: 0.0, code: 12345, errmsg: 'oh no!'}");
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(12345));
    ASSERT_EQ(status.reason(), "oh no!");
}

TEST(GetStatusFromCommandResult, SimpleNoCode) {
    const auto status = statusFor("{ok: 0.0, errmsg: 'oh no!'}");
    ASSERT_EQ(status, ErrorCodes::UnknownError);
    ASSERT_EQ(status.reason(), "oh no!");
    ASSERT(!status.extraInfo());
}

TEST(GetStatusFromCommandResult, ExtraInfoParserFails) {
    const auto status = statusFor("{ok: 0.0, code: 236, errmsg: 'oh no!', data: 123}");
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(40681));
    ASSERT(!status.extraInfo());
}

TEST(GetStatusFromCommandResult, ExtraInfoParserSucceeds) {
    ErrorExtraInfoExample::EnableParserForTest whenInScope;
    const auto status = statusFor("{ok: 0.0, code: 236, errmsg: 'oh no!', data: 123}");
    ASSERT_EQ(status, ErrorCodes::ForTestingErrorExtraInfo);
    ASSERT_EQ(status.reason(), "oh no!");
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<ErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<ErrorExtraInfoExample>()->data, 123);
}

TEST(GetStatusFromCommandResult, BulkWriteResponseSucceeds) {
    ASSERT_OK(getFirstWriteErrorStatusFromBulkWriteResult(
        fromjson("{ok: 1.0, cursor: {id: 0, firstBatch: [{ok: 1.0}, {ok: 1.0}]}}")));
}

TEST(GetStatusFromCommandResult, BulkWriteResponseFails) {
    ErrorExtraInfoExample::EnableParserForTest whenInScope;
    auto status = getFirstWriteErrorStatusFromBulkWriteResult(
        fromjson(("{ok: 1.0, cursor: {id: 0, firstBatch: [{ok: 1.0}, {ok: 0.0, code: 236, errmsg: "
                  "'oh no!', data: 123}]}}")));
    ASSERT_EQ(status, ErrorCodes::ForTestingErrorExtraInfo);
    ASSERT_EQ(status.reason(), "oh no!");
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<ErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<ErrorExtraInfoExample>()->data, 123);
}

TEST(GetStatusFromCommandResult, ErrorWithWriteConcernErrorInfo) {
    auto commandResultWithWCE = R"({
                "writeConcernError": {
                    "code": {
                        "$numberInt": "64"
                    },
                    "codeName": "WriteConcernTimeout",
                    "errmsg": "waiting for replication timed out",
                    "errInfo": {
                        "wtimeout": true,
                        "writeConcern": {
                            "w": {
                                "$numberInt": "3"
                            },
                            "wtimeout": {
                                "$numberInt": "500"
                            },
                            "provenance": "clientSupplied"
                        }
                    }
                },
                "ok": {
                    "$numberDouble": "0"
                },
                "errmsg": "Plan executor error during findAndModify :: caused by :: E11000 duplicate key error collection: test.user index: b_1 dup key: { b: 2.0 }",
                "code": {
                    "$numberInt": "11000"
                },
                "codeName": "DuplicateKey",
                "keyPattern":{"b":{"$numberDouble":"1"}},"keyValue":{"b":{"$numberDouble":"2"}}
            })";

    auto status = getStatusWithWCErrorDetailFromCommandResult(fromjson(commandResultWithWCE));
    ASSERT_EQ(status.code(), ErrorCodes::ErrorWithWriteConcernError);
    ASSERT(!status.reason().empty());
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<ErrorWithWriteConcernErrorInfo>());
    ASSERT(status.extraInfo<ErrorWithWriteConcernErrorInfo>()
               ->getWriteConcernErrorDetail()
               .isErrInfoSet());
    ASSERT_EQ(status.extraInfo<ErrorWithWriteConcernErrorInfo>()->getMainStatus().code(),
              ErrorCodes::DuplicateKey);
}

TEST(GetStatusFromCommandResult, WCEDetailAppendToCmdResp) {
    ShardId shardId("shard0");
    BSONObjBuilder bob;
    WriteConcernErrorDetail wce;
    std::string errMsg;
    auto wceObject = fromjson(R"(
    {
        "code": {
            "$numberInt": "64"
        },
        "codeName": "WriteConcernTimeout",
        "errmsg": "waiting for replication timed out",
        "errInfo": {
            "wtimeout": true,
            "writeConcern": {
                "w": {
                    "$numberInt": "3"
                },
                "wtimeout": {
                    "$numberInt": "500"
                },
                "provenance": "clientSupplied"
            }
        }
    })");

    wce.parseBSON(wceObject, &errMsg);
    ASSERT(errMsg.empty());

    appendWriteConcernErrorDetailToCommandResponse(shardId, wce, bob);
    auto resp = bob.done();

    WriteConcernErrorDetail newWCE;
    newWCE.parseBSON(resp[kWriteConcernErrorFieldName].Obj(), &errMsg);
    ASSERT(errMsg.empty());
    ASSERT(newWCE.toStatus().reason().find(shardId.toString()) != std::string::npos);

    // Test that WCE error is not appended twice.
    bob.abandon();
    bob.append(kWriteConcernErrorFieldName, wceObject);
    appendWriteConcernErrorDetailToCommandResponse(shardId, wce, bob);
    ASSERT_DOES_NOT_THROW(bob.done());
}

// namespace mongo
}  // namespace mongo
