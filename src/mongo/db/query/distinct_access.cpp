/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/distinct_access.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stage_types.h"

namespace mongo {

bool turnIxscanIntoDistinctIxscan(QuerySolution* soln,
                                  const std::string& field,
                                  bool strictDistinctOnly,
                                  bool flipDistinctScanDirection) {
    auto root = soln->root();

    // Temporarily check if the plan already contains a distinct scan. That happens in the
    // aggregation code path where this function is called twice for a place. The only possible
    // plans are FETCH + DISTINCT or PROJECT + DISTINCT.
    //
    // TODO SERVER-92615: Remove this.
    if (root->children.size() > 0 && root->children[0]->getType() == STAGE_DISTINCT_SCAN) {
        return true;
    }

    // We can attempt to convert a plan if it follows one of these patterns (starting from the
    // root):
    //   1. PROJECT=>FETCH=>IXSCAN
    //   2. FETCH=>IXSCAN
    //   3. PROJECT=>IXSCAN
    QuerySolutionNode* projectNode = nullptr;
    IndexScanNode* indexScanNode = nullptr;
    FetchNode* fetchNode = nullptr;

    switch (root->getType()) {
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE:
            projectNode = root;
            break;
        case STAGE_FETCH:
            fetchNode = static_cast<FetchNode*>(root);
            break;
        default:
            return false;
    }

    if (!fetchNode && (STAGE_FETCH == root->children[0]->getType())) {
        fetchNode = static_cast<FetchNode*>(root->children[0].get());
    }

    if (fetchNode && (STAGE_IXSCAN == fetchNode->children[0]->getType())) {
        indexScanNode = static_cast<IndexScanNode*>(fetchNode->children[0].get());
    } else if (projectNode && (STAGE_IXSCAN == projectNode->children[0]->getType())) {
        indexScanNode = static_cast<IndexScanNode*>(projectNode->children[0].get());
    }

    if (!indexScanNode) {
        return false;
    }

    // If the fetch has a filter, we're out of luck. We can't skip all keys with a given value,
    // since one of them may key a document that passes the filter.
    if (fetchNode && fetchNode->filter) {
        return false;
    }

    if (indexScanNode->index.type == IndexType::INDEX_WILDCARD) {
        // If the query is on a field other than the distinct key, we may have generated a $** plan
        // which does not actually contain the distinct key field.
        if (field != std::next(indexScanNode->index.keyPattern.begin())->fieldName()) {
            return false;
        }
        // If the query includes object bounds, we cannot turn this IXSCAN into a DISTINCT_SCAN.
        // Wildcard indexes contain multiple keys per object, one for each subpath in ascending
        // (Path, Value, RecordId) order. If the distinct fields in two successive documents are
        // objects with the same leaf path values but in different field order, e.g. {a: 1, b: 2}
        // and {b: 2, a: 1}, we would therefore only return the first document and skip the other.
        if (wildcard_planning::isWildcardObjectSubpathScan(indexScanNode)) {
            return false;
        }
    }

    // An additional filter must be applied to the data in the key, so we can't just skip
    // all the keys with a given value; we must examine every one to find the one that (may)
    // pass the filter.
    if (indexScanNode->filter) {
        return false;
    }

    // We only set this when we have special query modifiers (.max() or .min()) or other
    // special cases.  Don't want to handle the interactions between those and distinct.
    // Don't think this will ever really be true but if it somehow is, just ignore this
    // soln.
    if (indexScanNode->bounds.isSimpleRange) {
        return false;
    }

    // Figure out which field we're skipping to the next value of.
    int fieldNo = 0;
    BSONObjIterator it(indexScanNode->index.keyPattern);
    while (it.more()) {
        if (field == it.next().fieldName()) {
            break;
        }
        ++fieldNo;
    }

    if (strictDistinctOnly) {
        // If the "distinct" field is not the first field in the index bounds then the only way we
        // can guarantee that we'll never see duplicate values for the distinct field is to make
        // sure every field before the distinct field has equality bounds. For example, a
        // DISTINCT_SCAN on 'b' over the {a: 1, b: 1} index will scan a particular 'b' value
        // multiple times if that 'b' value exists in documents with different 'a' values. The
        // equality bounds on 'a' prevent the scan from seeing duplicate 'b' values by ensuring the
        // scan is limited to a single value for the 'a' field.
        for (size_t i = 0; i < static_cast<size_t>(fieldNo); ++i) {
            invariant(i < indexScanNode->bounds.size());
            if (indexScanNode->bounds.fields[i].intervals.size() != 1 ||
                !indexScanNode->bounds.fields[i].intervals[0].isPoint()) {
                return false;
            }
        }
    }

    // We should not use a distinct scan if the field over which we are computing the distinct is
    // multikey.
    if (indexScanNode->index.multikey) {
        const auto& multikeyPaths = indexScanNode->index.multikeyPaths;
        if (multikeyPaths.empty()) {
            // We don't have path-level multikey information available.
            return false;
        }

        if (!multikeyPaths[fieldNo].empty()) {
            // Path-level multikey information indicates that the distinct key contains at least one
            // array component.
            return false;
        }
    }

    // Make a new DistinctNode. We will swap this for the ixscan in the provided solution.
    auto distinctNode = std::make_unique<DistinctNode>(indexScanNode->index);
    distinctNode->direction =
        flipDistinctScanDirection ? -indexScanNode->direction : indexScanNode->direction;
    distinctNode->bounds =
        flipDistinctScanDirection ? indexScanNode->bounds.reverse() : indexScanNode->bounds;
    distinctNode->queryCollator = indexScanNode->queryCollator;
    distinctNode->fieldNo = fieldNo;

    if (fetchNode) {
        // If the original plan had PROJECT and FETCH stages, we can get rid of the PROJECT
        // transforming the plan from PROJECT=>FETCH=>IXSCAN to FETCH=>DISTINCT_SCAN.
        if (projectNode) {
            invariant(projectNode == root);
            invariant(fetchNode == root->children[0].get());
            invariant(STAGE_FETCH == root->children[0]->getType());
            invariant(STAGE_IXSCAN == root->children[0]->children[0]->getType());
            // Make the fetch the new root. This destroys the project stage.
            soln->setRoot(std::move(root->children[0]));
        }

        // Attach the distinct node in the index scan's place.
        fetchNode->children[0] = std::move(distinctNode);
    } else {
        // There is no fetch node. The PROJECT=>IXSCAN tree should become PROJECT=>DISTINCT_SCAN.
        invariant(projectNode == root);
        invariant(STAGE_IXSCAN == root->children[0]->getType());

        // Attach the distinct node in the index scan's place.
        root->children[0] = std::move(distinctNode);
    }

    return true;
}

}  // namespace mongo
