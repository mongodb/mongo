// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
