// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/action_type.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <iostream>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr std::string_view kAction = "action"sv;
}  // namespace

StatusWith<ActionType> parseActionFromString(std::string_view action) {
    try {
        return {idl::deserialize<ActionTypeEnum>(action, IDLParserContext(kAction))};
    } catch (DBException&) {
        // ignore
    }
    return Status(ErrorCodes::FailedToParse,
                  fmt::format("Unrecognized action privilege string: {}", action));
}

std::string_view toStringData(ActionType a) {
    return idl::serialize(a);
}

std::string toString(ActionType a) {
    return std::string{idl::serialize(a)};
}

std::ostream& operator<<(std::ostream& os, const ActionType& a) {
    return os << idl::serialize(a);
}

}  // namespace mongo
