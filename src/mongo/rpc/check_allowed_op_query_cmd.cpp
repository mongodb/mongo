// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/rpc/check_allowed_op_query_cmd.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <array>
#include <string_view>

#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;

void checkAllowedOpQueryCommand(Client& client, std::string_view cmd) {
    static constexpr std::array allowed{
        "hello"sv,
        "isMaster"sv,
        "ismaster"sv,
    };
    const bool isAllowed = (std::find(allowed.begin(), allowed.end(), cmd) != allowed.end());

    // The deprecated commands below are still used by some old drivers. Eventually, they should go.
    static constexpr std::array temporarilyAllowed{
        "_isSelf"sv,
        "authenticate"sv,
        "buildinfo"sv,
        "buildInfo"sv,
        "saslContinue"sv,
        "saslStart"sv,
    };
    const bool isTemporarilyAllowed =
        (std::find(temporarilyAllowed.begin(), temporarilyAllowed.end(), cmd) !=
         temporarilyAllowed.end());

    if (!isAllowed && !isTemporarilyAllowed) {
        uasserted(ErrorCodes::UnsupportedOpQueryCommand,
                  fmt::format(
                      "Unsupported OP_QUERY command: {}. The client driver may require an upgrade. "
                      "For more details see https://dochub.mongodb.org/core/legacy-opcode-removal",
                      cmd));
    }

    if (isTemporarilyAllowed) {
        globalOpCounters().gotQueryDeprecated();
    }
}

}  // namespace mongo
