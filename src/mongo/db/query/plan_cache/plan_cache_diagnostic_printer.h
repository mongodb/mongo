// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/util/modules.h"

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
