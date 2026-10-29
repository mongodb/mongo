// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <string_view>
#include <variant>

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {

using WTags = StringMap<std::int64_t>;
using WriteConcernW = std::variant<std::string, std::int64_t, WTags>;

// Helpers for IDL parsing
WriteConcernW deserializeWriteConcernW(BSONElement wEl);
void serializeWriteConcernW(const WriteConcernW& w,
                            std::string_view fieldName,
                            BSONObjBuilder* builder);
std::int64_t parseWTimeoutFromBSON(BSONElement element);
void serializeWTimeout(std::int64_t wTimeout, std::string_view fieldName, BSONObjBuilder* builder);

}  // namespace mongo
