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

#include "mongo/base/string_data.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/logv2/redaction.h"

#include <fmt/format.h>

namespace mongo::diagnostic_printers {

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

            auto sep = ""_sd;
            std::for_each(info.indexes.begin(), info.indexes.end(), [&](const IndexEntry& ie) {
                out = fmt::format_to(
                    out, "{}\"{}\"", std::exchange(sep, ", "_sd), redact(getIndexEntryStr(ie)));
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
