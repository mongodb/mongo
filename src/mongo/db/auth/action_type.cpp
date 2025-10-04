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

#include "mongo/db/auth/action_type.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <iostream>
#include <string>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

constexpr StringData kAction = "action"_sd;
}

StatusWith<ActionType> parseActionFromString(StringData action) {
    try {
        return {ActionType_parse(action, IDLParserContext(kAction))};
    } catch (DBException&) {
        // ignore
    }
    return Status(ErrorCodes::FailedToParse,
                  fmt::format("Unrecognized action privilege string: {}", action));
}

StringData toStringData(ActionType a) {
    return ActionType_serializer(a);
}

std::string toString(ActionType a) {
    return std::string{ActionType_serializer(a)};
}

std::ostream& operator<<(std::ostream& os, const ActionType& a) {
    return os << ActionType_serializer(a);
}

}  // namespace mongo
