// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::diagnostic_printers {
using namespace std::literals::string_view_literals;

constexpr inline std::string_view kOmitForUnshardedCollectionMsg{
    "omitted: collection isn't sharded"};

struct ShardKeyDiagnosticPrinter {
    ShardKeyDiagnosticPrinter(const BSONObj& shardKeyPattern) {
        if (!shardKeyPattern.isEmpty()) {
            // No need to potentially create a copy of an empty BSONObj.
            shardKey = shardKeyPattern.getOwned();
        }
    }

    auto format(auto& fc) const {
        auto out = fc.out();
        if (!shardKey.has_value()) {
            return fmt::format_to(out, "{}", kOmitForUnshardedCollectionMsg);
        }
        out = fmt::format_to(out, "{{'shardKeyPattern': {}}}", shardKey->toString());
        return out;
    }

    // This BSONObj is owned.
    boost::optional<BSONObj> shardKey;
};

struct MultipleShardKeysDiagnosticPrinter {
    MultipleShardKeysDiagnosticPrinter(
        stdx::unordered_map<NamespaceString, boost::optional<BSONObj>>& inputMap) {
        for (const auto& [ns, shardKey] : inputMap) {
            namespaceToShardKeyMap.insert({ns,
                                           shardKey.has_value()
                                               ? boost::optional<BSONObj>(shardKey->getOwned())
                                               : boost::none});
        }
    }

    auto format(auto& fc) const {
        auto out = fc.out();
        out = fmt::format_to(out, "{{");
        auto sep = ""sv;
        for (const auto& [ns, shardKey] : namespaceToShardKeyMap) {
            out = fmt::format_to(
                out,
                "{}'{}': {{'shardKeyPattern': {}}}",
                std::exchange(sep, ", "sv),
                NamespaceStringUtil::serialize(ns, SerializationContext::stateDefault()),
                shardKey.has_value() ? shardKey.value().toString()
                                     : kOmitForUnshardedCollectionMsg);
        }
        out = fmt::format_to(out, "}}");
        return out;
    }

    // Any BSONObjs in this map are owned.
    stdx::unordered_map<NamespaceString, boost::optional<BSONObj>> namespaceToShardKeyMap;
};

}  // namespace mongo::diagnostic_printers

namespace fmt {
template <>
struct formatter<mongo::diagnostic_printers::ShardKeyDiagnosticPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::diagnostic_printers::ShardKeyDiagnosticPrinter& obj, auto& ctx) const {
        return obj.format(ctx);
    }
};

template <>
struct formatter<mongo::diagnostic_printers::MultipleShardKeysDiagnosticPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::diagnostic_printers::MultipleShardKeysDiagnosticPrinter& obj,
                auto& ctx) const {
        return obj.format(ctx);
    }
};
}  // namespace fmt
