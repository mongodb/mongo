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
