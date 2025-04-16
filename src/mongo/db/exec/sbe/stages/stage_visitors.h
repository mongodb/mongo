/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/ix_scan.h"

namespace mongo::sbe {
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
        if (stats->stageType == "seek"_sd) {
            collectionSeeks += stats->opens;
        } else if (stats->stageType == "scan"_sd) {
            collectionScans += stats->opens;
        } else if (stats->stageType == "ixseek"_sd || stats->stageType == "ixscan"_sd) {
            auto indexScanStage = checked_cast<const SimpleIndexScanStage*>(root);
            indexesUsed.push_back(indexScanStage->getIndexName());
            if (stats->stageType == "ixseek"_sd) {
                indexSeeks += stats->opens;
            } else if (stats->stageType == "ixscan"_sd) {
                indexScans += stats->opens;
            }
        } else if (stats->stageType == "ixscan_generic"_sd) {
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
