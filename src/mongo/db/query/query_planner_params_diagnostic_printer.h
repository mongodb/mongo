// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_planner_params.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::diagnostic_printers {
using namespace std::literals::string_view_literals;

/**
 * Diagnostic printer for QueryPlannerParams. For example, it includes indexes considered during
 * query planning.
 */
struct QueryPlannerParamsPrinter {
    auto format(const auto& fc) const {
        auto out = fc.out();

        auto writeCollInfo = [&](const std::string& ns, const CollectionInfo& info) {
            out = fmt::format_to(out,
                                 "{}: {{exists: {}, isTimeseries: {}, indexes: [",
                                 ns,
                                 info.exists,
                                 info.stats.isTimeseries);

            auto sep = ""sv;
            std::for_each(info.indexes.begin(), info.indexes.end(), [&](const IndexEntry& ie) {
                out = fmt::format_to(
                    out, "{}\"{}\"", std::exchange(sep, ", "sv), redact(getIndexEntryStr(ie)));
            });
            out = fmt::format_to(out, "]}}");
        };

        out = fmt::format_to(out, "{{");

        writeCollInfo("mainCollectionInfo", plannerParams.mainCollectionInfo);

        for (const auto& [ns, collInfo] : plannerParams.secondaryCollectionsInfo) {
            out = fmt::format_to(out, ", ");
            writeCollInfo(ns.toStringForErrorMsg(), collInfo);
        }

        out = fmt::format_to(out, "}}");
        return out;
    }

    const QueryPlannerParams& plannerParams;

private:
    // 'IndexEntry' lifetimes aren't tied to the underlying collection, but some of its data members
    // are tied to the underlying indexes. Here we serialize an IndexEntry like in
    // IndexEntry::toString(), but we only print the owned members.
    std::string getIndexEntryStr(const IndexEntry& ie) const {
        StringBuilder sb;

        sb << "name: '" << ie.identifier << "'";
        sb << " type: " << ie.type;

        if (ie.keyPattern.isOwned()) {
            sb << " kp: " << ie.keyPattern;
        } else {
            sb << " kp: unowned";
        }

        if (ie.multikey) {
            sb << " multikey";
        }

        if (ie.sparse) {
            sb << " sparse";
        }

        if (ie.unique) {
            sb << " unique";
        }

        if (ie.filterExpr) {
            sb << " partial";
        }

        if (ie.infoObj.isOwned()) {
            sb << " io: " << ie.infoObj;
        } else {
            sb << " io: unowned";
        }

        if (ie.collator) {
            sb << " hasCollator";
        }

        return sb.str();
    }
};

}  // namespace mongo::diagnostic_printers

namespace fmt {

template <>
struct formatter<mongo::diagnostic_printers::QueryPlannerParamsPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::diagnostic_printers::QueryPlannerParamsPrinter& obj, auto& ctx) const {
        return obj.format(ctx);
    }
};

}  // namespace fmt
