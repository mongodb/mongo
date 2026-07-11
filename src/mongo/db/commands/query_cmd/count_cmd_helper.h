// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/count_command_gen.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace count_cmd_helper {

/**
 * Builds a CountCommandReply from 'countResult', selecting the appropriate integer width (int32 for
 * values < 2^31, otherwise int64) for the count field.
 */
inline CountCommandReply buildCountReply(long long countResult) {
    uassert(7145301, "count value must not be negative", countResult >= 0);

    // Return either BSON int32 or int64, depending on the value of 'countResult'. We encode it as
    // int32 when 'countResult' < 2^31, and as an int64 otherwise. This keeps small count output
    // looking like {n: 2} instead of {n: Long('2')} which some client applications may still rely
    // on.
    auto count = [](long long countResult) -> std::variant<std::int32_t, std::int64_t> {
        constexpr long long maxIntCountResult = std::numeric_limits<std::int32_t>::max();
        if (countResult < maxIntCountResult) {
            return static_cast<std::int32_t>(countResult);
        }
        return static_cast<std::int64_t>(countResult);
    }(countResult);

    CountCommandReply reply;
    reply.setCount(count);
    return reply;
}

}  // namespace count_cmd_helper
}  // namespace mongo
