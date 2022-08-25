/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/idl/command_generic_argument.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace test {

using namespace fmt::literals;

// A copy of the generic command arguments and reply fields from before they were moved to IDL in
// SERVER-51848. We will test that the IDL definitions match these old C++ definitions.
struct SpecialArgRecord {
    StringData name;
    bool isGenericArgument;
    bool isGenericReply;
    bool stripFromRequest;
    bool stripFromReply;
};

// clang-format off
static constexpr std::array<SpecialArgRecord, 35> specials{{
    //                                       /-isGenericArgument
    //                                       |  /-isGenericReply
    //                                       |  |  /-stripFromRequest
    //                                       |  |  |  /-stripFromReply
    {"apiVersion"_sd,                        1, 0, 1, 0},
    {"apiStrict"_sd,                         1, 0, 1, 0},
    {"apiDeprecationErrors"_sd,              1, 0, 1, 0},
    {"$audit"_sd,                            1, 0, 1, 0},
    {"$client"_sd,                           1, 0, 1, 0},
    {"$configServerState"_sd,                1, 1, 1, 1},
    {"$db"_sd,                               1, 0, 1, 0},
    {"allowImplicitCollectionCreation"_sd,   1, 0, 1, 0},
    {"$oplogQueryData"_sd,                   1, 1, 1, 1},
    {"$queryOptions"_sd,                     1, 0, 0, 0},
    {"$readPreference"_sd,                   1, 0, 1, 0},
    {"$replData"_sd,                         1, 1, 1, 1},
    {"$clusterTime"_sd,                      1, 1, 1, 1},
    {"maxTimeMS"_sd,                         1, 0, 0, 0},
    {"readConcern"_sd,                       1, 0, 0, 0},
    {"databaseVersion"_sd,                   1, 0, 1, 0},
    {"shardVersion"_sd,                      1, 0, 1, 0},
    {"tracking_info"_sd,                     1, 0, 1, 0},
    {"writeConcern"_sd,                      1, 0, 0, 0},
    {"lsid"_sd,                              1, 0, 0, 0},
    {"clientOperationKey"_sd,                1, 0, 0, 0},
    {"txnNumber"_sd,                         1, 0, 0, 0},
    {"autocommit"_sd,                        1, 0, 0, 0},
    {"coordinator"_sd,                       1, 0, 0, 0},
    {"startTransaction"_sd,                  1, 0, 0, 0},
    {"stmtId"_sd,                            1, 0, 0, 0},
    {"$gleStats"_sd,                         0, 1, 0, 1},
    {"operationTime"_sd,                     0, 1, 0, 1},
    {"lastCommittedOpTime"_sd,               0, 1, 0, 1},
    {"readOnly"_sd,                          0, 1, 0, 1},
    {"comment"_sd,                           1, 0, 0, 0},
    {"maxTimeMSOpOnly"_sd,                   1, 0, 1, 0},
    {"$configTime"_sd,                       1, 1, 1, 1},
    {"ok"_sd,                                0, 1, 0, 0},
    {"$topologyTime"_sd,                     1, 1, 1, 1}}};
// clang-format on

TEST(CommandGenericArgument, AllGenericArgumentsAndReplyFields) {
    for (const auto& record : specials) {
        if (isGenericArgument(record.name) != record.isGenericArgument) {
            FAIL("isGenericArgument('{}') should be {}, but it's {}"_format(
                record.name, record.isGenericArgument, isGenericArgument(record.name)));
        }

        if (isGenericReply(record.name) != record.isGenericReply) {
            FAIL("isGenericReply('{}') should be {}, but it's {}"_format(
                record.name, record.isGenericReply, isGenericReply(record.name)));
        }

        if (shouldForwardToShards(record.name) == record.stripFromRequest) {
            FAIL("shouldForwardToShards('{}') should be {}, but it's {}"_format(
                record.name, !record.stripFromRequest, shouldForwardToShards(record.name)));
        }

        if (shouldForwardFromShards(record.name) == record.stripFromReply) {
            FAIL("shouldForwardFromShards('{}') should be {}, but it's {}"_format(
                record.name, !record.stripFromReply, shouldForwardFromShards(record.name)));
        }
    }
}
}  // namespace test
}  // namespace mongo
