// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/write_concern_idl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {
// Helpers for IDL parsing
WriteConcernW deserializeWriteConcernW(BSONElement wEl) {
    if (wEl.isNumber()) {
        uassert(ErrorCodes::FailedToParse, "w cannot be NaN", !wEl.isNaN());
        auto wNum = wEl.safeNumberLong();
        if (wNum < 0 || wNum > static_cast<long long>(repl::ReplSetConfig::kMaxMembers)) {
            uasserted(
                ErrorCodes::FailedToParse,
                fmt::format("w has to be a non-negative number and not greater than {}; found: {}",
                            repl::ReplSetConfig::kMaxMembers,
                            wNum));
        }

        return WriteConcernW{wNum};
    } else if (wEl.type() == BSONType::string) {
        auto wStr = wEl.str();
        uassert(ErrorCodes::FailedToParse,
                fmt::format("w has illegal embedded NUL byte, w: {}", wStr),
                wStr.find('\0') == std::string::npos);
        return WriteConcernW{std::move(wStr)};
    } else if (wEl.type() == BSONType::object) {
        auto wTags = wEl.Obj();
        uassert(ErrorCodes::FailedToParse, "tagged write concern requires tags", !wTags.isEmpty());

        WTags tags;
        for (auto&& e : wTags) {
            uassert(ErrorCodes::FailedToParse,
                    fmt::format(
                        "tags must be a single level document with only number values; found: {}",
                        e.toString()),
                    e.isNumber());

            tags.try_emplace(e.fieldName(), e.safeNumberInt());
        }

        return WriteConcernW{std::move(tags)};
    } else if (wEl.eoo() || wEl.type() == BSONType::null || wEl.type() == BSONType::undefined) {
        return WriteConcernW{};
    }
    uasserted(
        ErrorCodes::FailedToParse,
        fmt::format("w has to be a number, string, or object; found: {}", typeName(wEl.type())));
}

void serializeWriteConcernW(const WriteConcernW& w,
                            std::string_view fieldName,
                            BSONObjBuilder* builder) {
    visit(OverloadedVisitor{[&](int64_t wNumNodes) {
                                builder->appendNumber(fieldName, static_cast<long long>(wNumNodes));
                            },
                            [&](const std::string& wMode) { builder->append(fieldName, wMode); },
                            [&](WTags wTags) {
                                builder->append(fieldName, wTags);
                            }},
          w);
}

std::int64_t parseWTimeoutFromBSON(BSONElement element) {
    // Store wTimeout as a 64-bit value but functionally limit it to int32 as values larger than
    // than that do not make much sense to use and were not previously supported.
    constexpr std::array validTypes{
        BSONType::numberLong, BSONType::numberInt, BSONType::numberDecimal, BSONType::numberDouble};
    bool isValidType = std::any_of(
        validTypes.begin(), validTypes.end(), [&](auto type) { return element.type() == type; });

    auto value = isValidType ? element.safeNumberLong() : 0;
    uassert(ErrorCodes::FailedToParse,
            "wtimeout must be a 32-bit integer",
            value <= std::numeric_limits<int32_t>::max());
    return value;
}

void serializeWTimeout(std::int64_t wTimeout, std::string_view fieldName, BSONObjBuilder* builder) {
    // Historically we have serialized this as a int32_t, even though it is defined as an
    // int64_t in our IDL format.
    builder->append(fieldName, static_cast<int32_t>(wTimeout));
}

}  // namespace mongo
