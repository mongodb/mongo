/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/stats/counters.h"
#include "mongo/rpc/check_allowed_op_query_cmd.h"

#include <fmt/format.h>
#include <string>

namespace mongo {

using namespace fmt::literals;

void checkAllowedOpQueryCommand(Client& client, StringData cmd) {
    static constexpr std::array allowed{
        "hello"_sd,
        "isMaster"_sd,
        "ismaster"_sd,
    };
    const bool isAllowed = (std::find(allowed.begin(), allowed.end(), cmd) != allowed.end());

    // The deprecated commands below are still used by some old drivers. Eventually, they should go.
    static constexpr std::array temporarilyAllowed{
        "_isSelf"_sd,
        "authenticate"_sd,
        "buildinfo"_sd,
        "buildInfo"_sd,
        "saslContinue"_sd,
        "saslStart"_sd,
    };
    const bool isTemporarilyAllowed =
        (std::find(temporarilyAllowed.begin(), temporarilyAllowed.end(), cmd) !=
         temporarilyAllowed.end());

    if (!isAllowed && !isTemporarilyAllowed) {
        uasserted(
            ErrorCodes::UnsupportedOpQueryCommand,
            "Unsupported OP_QUERY command: {}. The client driver may require an upgrade. "
            "For more details see https://dochub.mongodb.org/core/legacy-opcode-removal"_format(
                cmd));
    }

    if (isTemporarilyAllowed) {
        globalOpCounters.gotQueryDeprecated();
    }
}

}  // namespace mongo
