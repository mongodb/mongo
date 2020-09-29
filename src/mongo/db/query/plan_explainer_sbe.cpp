/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_explainer_sbe.h"

#include <queue>

#include "mongo/db/keypattern.h"

namespace mongo {
std::string PlanExplainerSBE::getPlanSummary() const {
    if (!_solution) {
        return {};
    }

    StringBuilder sb;
    std::queue<const QuerySolutionNode*> queue;
    queue.push(_solution->root());

    while (!queue.empty()) {
        auto node = queue.front();
        queue.pop();

        sb << stageTypeToString(node->getType());

        switch (node->getType()) {
            case STAGE_COUNT_SCAN: {
                auto csn = static_cast<const CountScanNode*>(node);
                const KeyPattern keyPattern{csn->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_DISTINCT_SCAN: {
                auto dn = static_cast<const DistinctNode*>(node);
                const KeyPattern keyPattern{dn->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_GEO_NEAR_2D: {
                auto geo2d = static_cast<const GeoNear2DNode*>(node);
                const KeyPattern keyPattern{geo2d->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_GEO_NEAR_2DSPHERE: {
                auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);
                const KeyPattern keyPattern{geo2dsphere->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_IXSCAN: {
                auto ixn = static_cast<const IndexScanNode*>(node);
                const KeyPattern keyPattern{ixn->index.keyPattern};
                sb << " " << keyPattern;
                break;
            }
            case STAGE_TEXT: {
                auto tn = static_cast<const TextNode*>(node);
                const KeyPattern keyPattern{tn->indexPrefix};
                sb << " " << keyPattern;
                break;
            }
            default:
                break;
        }

        for (auto&& child : node->children) {
            queue.push(child);
        }

        if (!queue.empty()) {
            sb << ", ";
        }
    }

    return sb.str();
}

void PlanExplainerSBE::getSummaryStats(PlanSummaryStats* statsOut) const {
    // TODO: SERVER-50744
}

PlanExplainer::PlanStatsDetails PlanExplainerSBE::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    // TODO: SERVER-50728
    return {{}, PlanSummaryStats{}};
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerSBE::getRejectedPlansStats(
    ExplainOptions::Verbosity verbosity) const {
    // TODO: SERVER-50728
    return {};
}

std::vector<PlanExplainer::PlanStatsDetails> PlanExplainerSBE::getCachedPlanStats(
    const PlanCacheEntry::DebugInfo&, ExplainOptions::Verbosity) const {
    // TODO: SERVER-50728
    return {};
}
}  // namespace mongo
