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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"

#include <cstdint>
#include <fmt/format.h>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

namespace {

struct ActionEntry {
    StringData name;
    ActionType action;
};

constexpr std::array kTable{
#define X_(a) ActionEntry{#a ""_sd, ActionType::a},
    EXPAND_ACTION_TYPE(X_)
#undef X_
};

constexpr bool isTableIndexedProperly() {
    if (kTable.size() != kNumActionTypes)
        return false;
    for (size_t i = 0; i < kTable.size(); ++i)
        if (kTable[i].action != static_cast<ActionType>(i))
            return false;
    return true;
}
static_assert(isTableIndexedProperly());

}  // namespace

StatusWith<ActionType> parseActionFromString(StringData action) {
    static const StaticImmortal byName = [] {
        std::map<StringData, ActionType> m;
        for (auto&& e : kTable)
            m.insert({e.name, e.action});
        return m;
    }();
    if (auto iter = byName->find(action); iter != byName->end()) {
        return iter->second;
    }
    return Status(ErrorCodes::FailedToParse,
                  fmt::format("Unrecognized action privilege string: {}", action));
}

StringData toStringData(ActionType a) {
    return kTable[static_cast<size_t>(a)].name;
}

std::string toString(ActionType a) {
    return std::string{toStringData(a)};
}

std::ostream& operator<<(std::ostream& os, const ActionType& a) {
    return os << toStringData(a);
}

}  // namespace mongo
