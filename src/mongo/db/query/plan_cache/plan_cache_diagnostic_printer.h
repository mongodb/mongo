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

#include "mongo/db/query/plan_cache/classic_plan_cache.h"

#include <fmt/format.h>

namespace mongo::diagnostic_printers {

/**
 * An example of what this might look like (if we cached a plan that uses the {a: 1} index):
 * PlanCacheDiagnostics: {'planCacheSolution':
 *      (index-tagged expression tree: tree=Leaf (a_1, ), pos: 0, can combine? 1\n)
 * }
 */
struct PlanCacheDiagnosticPrinter {
    auto format(auto& fc) const {
        auto out = fc.out();
        out =
            fmt::format_to(out, "{{'planCacheSolution': {}}}", getSolutionCacheDataStr(cacheData));
        return out;
    }

    // This must outlive this class.
    const SolutionCacheData& cacheData;

private:
    // We define a new serialization function for PlanCacheIndexTree in order to ensure that during
    // logging time we only ever print members that are owned. This future-proofs any modification
    // to PlanCacheIndexTree::toString.
    std::string getPlanCacheIndexTreeStr(const PlanCacheIndexTree& tree, int indents = 0) const {
        StringBuilder result;
        if (!tree.children.empty()) {
            result << std::string(3 * indents, '-') << "Node\n";
            int newIndent = indents + 1;
            for (auto it = tree.children.begin(); it != tree.children.end(); ++it) {
                result << getPlanCacheIndexTreeStr(**it, newIndent);
            }
            return result.str();
        } else {
            result << std::string(3 * indents, '-') << "Leaf ";
            if (nullptr != tree.entry.get()) {
                result << tree.entry->identifier << ", pos: " << tree.index_pos << ", can combine? "
                       << tree.canCombineBounds;
            }
            for (const auto& orPushdown : tree.orPushdowns) {
                result << "Move to ";
                bool firstPosition = true;
                for (auto position : orPushdown.route) {
                    if (!firstPosition) {
                        result << ",";
                    }
                    firstPosition = false;
                    result << position;
                }
                result << ": " << orPushdown.indexEntryId << " pos: " << orPushdown.position
                       << ", can combine? " << orPushdown.canCombineBounds << ". ";
            }
            result << '\n';
        }
        return result.str();
    }

    // We define a new serialization function for SolutionCacheData in order to ensure that during
    // logging time we only ever print members that are owned. This future-proofs any modification
    // to SolutionCacheData::toString.
    std::string getSolutionCacheDataStr(const SolutionCacheData& scd) const {
        // Note that below we check to see if the PlanCacheIndexTree pointer is null - while we
        // don't ever expect it to be null on the happy path, we check this anyway in case something
        // bad has happened to the pointer. For example, if we hit some tassert because it
        // was nullptr, accessing it here would be dangerous. We do something similar in the
        // CurOpPrinter.
        switch (scd.solnType) {
            case SolutionCacheData::SolutionType::COLLSCAN_SOLN:
                return "(collection scan)";
            case SolutionCacheData::SolutionType::VIRTSCAN_SOLN:
                return "(virtual scan)";
            case SolutionCacheData::SolutionType::WHOLE_IXSCAN_SOLN: {
                std::string treeStr;
                if (scd.tree.get() == nullptr) {
                    treeStr = "pointer to solution tree is invalid";
                } else {
                    treeStr = getPlanCacheIndexTreeStr(*scd.tree);
                }
                return str::stream() << "(whole index scan solution: "
                                     << "dir=" << scd.wholeIXSolnDir << "; "
                                     << "tree=" << treeStr << ")";
            }
            case SolutionCacheData::SolutionType::USE_INDEX_TAGS_SOLN: {
                std::string treeStr;
                if (scd.tree.get() == nullptr) {
                    treeStr = "pointer to solution tree is invalid";
                } else {
                    treeStr = getPlanCacheIndexTreeStr(*scd.tree);
                }
                return str::stream() << "(index-tagged expression tree: "
                                     << "tree=" << treeStr << ")";
            }
        }
        return "encountered an invalid solution type";
    }
};

}  // namespace mongo::diagnostic_printers

namespace fmt {
template <>
struct formatter<mongo::diagnostic_printers::PlanCacheDiagnosticPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::diagnostic_printers::PlanCacheDiagnosticPrinter& obj,
                auto& ctx) const {
        return obj.format(ctx);
    }
};
}  // namespace fmt
