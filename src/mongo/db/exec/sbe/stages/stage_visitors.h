// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/util/modules.h"

namespace mongo::sbe {
using namespace std::literals::string_view_literals;
/**
 * A stage visitor that accumulates the storage-related statistics inside itself when used for
 * walking over the SBE AST.
 */
class StorageAccessStatsVisitor : public PlanStageVisitor {
public:
    /**
     * Adds the collected storage-related statistics into the given 'bob'.
     */
    BSONObj toBSON() {
        BSONObjBuilder bob;
        bob.appendNumber("totalDocsExamined", totalDocsExamined);
        bob.appendNumber("totalKeysExamined", totalKeysExamined);
        bob.appendNumber("collectionScans", collectionScans);
        bob.appendNumber("collectionSeeks", collectionSeeks);
        bob.appendNumber("indexScans", indexScans);
        bob.appendNumber("indexSeeks", indexSeeks);
        bob.append("indexesUsed", indexesUsed);
        return bob.obj();
    }

    /**
     * Collects the storage-related statictics for the given 'root' and 'rootStats'.
     */
    static StorageAccessStatsVisitor collectStats(const PlanStage& root,
                                                  const PlanStageStats& rootStats) {
        StorageAccessStatsVisitor res;
        root.accumulate(res);
        auto joinSummary = sbe::collectExecutionStatsSummary(rootStats);
        res.totalDocsExamined = static_cast<long long>(joinSummary.totalDocsExamined);
        res.totalKeysExamined = static_cast<long long>(joinSummary.totalKeysExamined);
        return res;
    }

protected:
    void visit(const sbe::PlanStage* root) override {
        auto stats = root->getCommonStats();
        if (stats->stageType == "fetch"sv) {
            collectionSeeks += stats->advances;
        } else if (stats->stageType == "scan"sv) {
            collectionScans += stats->opens;
        } else if (stats->stageType == "ixseek"sv || stats->stageType == "ixscan"sv) {
            auto indexScanStage = checked_cast<const SimpleIndexScanStage*>(root);
            indexesUsed.push_back(indexScanStage->getIndexName());
            if (stats->stageType == "ixseek"sv) {
                indexSeeks += stats->opens;
            } else if (stats->stageType == "ixscan"sv) {
                indexScans += stats->opens;
            }
        } else if (stats->stageType == "ixscan_generic"sv) {
            auto indexScanStage = checked_cast<const GenericIndexScanStage*>(root);
            indexesUsed.push_back(indexScanStage->getIndexName());
            indexSeeks += stats->opens;
        }
    }

private:
    long long totalDocsExamined = 0;
    long long totalKeysExamined = 0;
    long long collectionScans = 0;
    long long collectionSeeks = 0;
    long long indexScans = 0;
    long long indexSeeks = 0;
    std::vector<std::string> indexesUsed;
};
}  // namespace mongo::sbe
