/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/namespace_string_util.h"

#include <fmt/format.h>

namespace mongo::diagnostic_printers {

constexpr inline auto kOmitForUnshardedCollectionMsg = "omitted: collection isn't sharded"_sd;

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
        auto sep = ""_sd;
        for (const auto& [ns, shardKey] : namespaceToShardKeyMap) {
            out = fmt::format_to(
                out,
                "{}'{}': {}",
                std::exchange(sep, ", "_sd),
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
