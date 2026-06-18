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

#include "mongo/idl/command_generic_argument.h"

#include "mongo/unittest/unittest.h"

#include <array>
#include <memory>
#include <string_view>

#include <fmt/format.h>

namespace mongo {
namespace test {
using namespace std::literals::string_view_literals;

// A copy of the generic command arguments and reply fields from before they were moved to IDL in
// SERVER-51848. We will test that the IDL definitions match these old C++ definitions.
struct SpecialArgRecord {
    std::string_view name;
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
    {"apiVersion"sv,                        1, 0, 1, 0},
    {"apiStrict"sv,                         1, 0, 1, 0},
    {"apiDeprecationErrors"sv,              1, 0, 1, 0},
    {"$audit"sv,                            1, 0, 1, 0},
    {"$client"sv,                           1, 0, 1, 0},
    {"$configServerState"sv,                1, 1, 1, 1},
    {"$db"sv,                               1, 0, 1, 0},
    {"$oplogQueryData"sv,                   1, 1, 1, 1},
    {"$queryOptions"sv,                     1, 0, 0, 0},
    {"$readPreference"sv,                   1, 0, 1, 0},
    {"$replData"sv,                         1, 1, 1, 1},
    {"$clusterTime"sv,                      1, 1, 1, 1},
    {"maxTimeMS"sv,                         1, 0, 0, 0},
    {"readConcern"sv,                       1, 0, 0, 0},
    {"databaseVersion"sv,                   1, 0, 1, 0},
    {"shardVersion"sv,                      1, 0, 1, 0},
    {"tracking_info"sv,                     1, 0, 1, 0},
    {"writeConcern"sv,                      1, 0, 0, 0},
    {"lsid"sv,                              1, 0, 0, 0},
    {"clientOperationKey"sv,                1, 0, 0, 0},
    {"txnNumber"sv,                         1, 0, 0, 0},
    {"autocommit"sv,                        1, 0, 0, 0},
    {"coordinator"sv,                       1, 0, 0, 0},
    {"startTransaction"sv,                  1, 0, 0, 0},
    {"stmtId"sv,                            1, 0, 0, 0},
    {"$gleStats"sv,                         0, 1, 0, 1},
    {"operationTime"sv,                     0, 1, 0, 1},
    {"lastCommittedOpTime"sv,               0, 1, 0, 1},
    {"readOnly"sv,                          0, 1, 0, 1},
    {"comment"sv,                           1, 0, 0, 0},
    {"maxTimeMSOpOnly"sv,                   1, 0, 1, 0},
    {"$configTime"sv,                       1, 1, 1, 1},
    {"ok"sv,                                0, 1, 0, 0},
    {"$topologyTime"sv,                     1, 1, 1, 1},
    {"$traceCtx"sv,                         1, 0, 0, 0}}};
// clang-format on

TEST(CommandGenericArgument, AllGenericArgumentsAndReplyFields) {
    for (const auto& record : specials) {
        if (isGenericArgument(record.name) != record.isGenericArgument) {
            FAIL(fmt::format("isGenericArgument('{}') should be {}, but it's {}",
                             record.name,
                             record.isGenericArgument,
                             isGenericArgument(record.name)));
        }

        if (isGenericReply(record.name) != record.isGenericReply) {
            FAIL(fmt::format("isGenericReply('{}') should be {}, but it's {}",
                             record.name,
                             record.isGenericReply,
                             isGenericReply(record.name)));
        }

        if (shouldForwardToShards(record.name) == record.stripFromRequest) {
            FAIL(fmt::format("shouldForwardToShards('{}') should be {}, but it's {}",
                             record.name,
                             !record.stripFromRequest,
                             shouldForwardToShards(record.name)));
        }

        if (shouldForwardFromShards(record.name) == record.stripFromReply) {
            FAIL(fmt::format("shouldForwardFromShards('{}') should be {}, but it's {}",
                             record.name,
                             !record.stripFromReply,
                             shouldForwardFromShards(record.name)));
        }
    }
}
}  // namespace test
}  // namespace mongo
