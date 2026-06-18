/**
 * Shared primitives for verifying multikey metadata consistency across replica set members
 * at a clusterTime.
 */

export const IndexType = Object.freeze({REGULAR: "regular", WILDCARD: "wildcard"});

/**
 * Collection-scoped $listCatalog snapshot-read at `atClusterTime`. Collection scope (rather
 * than admin scope) avoids traversing sibling collections/views; tests that intentionally
 * create invalid views (e.g. InvalidViewDefinition) won't fail the scan.
 *
 * Returns:
 *   - Array<IndexMetadata>: `md.indexes` on success
 *   - null: catalog entry absent at `atClusterTime` (collection doesn't exist yet)
 *   - undefined: transient error (snapshot too old, namespace gone mid-call); caller should skip
 */
export function readCatalogIndexesAtClusterTime(node, dbName, collName, atClusterTime) {
    let res;
    try {
        res = node.getDB(dbName).runCommand({
            aggregate: collName,
            pipeline: [{$listCatalog: {}}],
            cursor: {batchSize: 1},
            readConcern: {level: "snapshot", atClusterTime: atClusterTime},
        });
    } catch (e) {
        return undefined;
    }
    if (!res.ok) return undefined;
    const docs = res.cursor.firstBatch;
    if (docs.length === 0) return null;
    const md = docs[0].md;
    if (!md || !md.indexes) return null;
    return md.indexes;
}

export function findIndexByName(indexes, indexName) {
    return indexes.find((i) => i.spec && i.spec.name === indexName);
}

/**
 * Locates the index covering `fieldName` for the given IndexType:
 *   - WILDCARD: matches the unique index with keyPattern {"$**": 1}.
 *   - REGULAR: matches a single-field index whose keyPattern is exactly {fieldName: 1}.
 */
export function findIndexForField(indexes, indexType, fieldName) {
    if (indexType === IndexType.WILDCARD) {
        return indexes.find((idx) => idx.spec && idx.spec.key && idx.spec.key["$**"] === 1);
    }
    return indexes.find((idx) => {
        const key = idx.spec && idx.spec.key;
        if (!key) return false;
        const keys = Object.keys(key);
        return keys.length === 1 && key[fieldName] === 1;
    });
}

/**
 * Walks an explain tree (both classic and SBE) and returns the wildcard IXSCAN node, or
 * null if none. Classic: winningPlan.{stage, inputStage}. SBE wraps the QuerySolutionNode
 * tree under a 'queryPlan' field; recurse into it.
 */
export function findWildcardIxscanNode(plan) {
    if (!plan) return null;
    if (plan.stage === "IXSCAN" && plan.keyPattern && plan.keyPattern["$_path"]) return plan;
    if (plan.queryPlan) {
        const r = findWildcardIxscanNode(plan.queryPlan);
        if (r) return r;
    }
    if (plan.inputStage) return findWildcardIxscanNode(plan.inputStage);
    if (plan.inputStages) {
        for (const s of plan.inputStages) {
            const r = findWildcardIxscanNode(s);
            if (r) return r;
        }
    }
    return null;
}

/**
 * Explain-based read of wildcard multikey paths for a single field at snapshot `T`. Uses
 * an equality predicate to force the planner to compute bounds for `fieldName`.
 *
 * Returns:
 *   - Array<String>: paths for `fieldName` from the IXSCAN node (may be empty)
 *   - undefined: explain failed transiently; caller should skip
 *
 * A missing IXSCAN node returns [] (planner picked a different plan; treat as no data so
 * cross-member compares agree on emptiness rather than asserting).
 */
export function readWildcardMultikeyPaths(node, dbName, collName, T, hint, fieldName) {
    const filter = {};
    filter[fieldName] = 1;
    let res;
    try {
        res = node.getDB(dbName).runCommand({
            explain: {find: collName, filter: filter, hint: hint},
            readConcern: {level: "snapshot", atClusterTime: T},
            verbosity: "queryPlanner",
        });
    } catch (e) {
        // A thrown exception (e.g. stepdown, network error) is not a divergence; skip this member.
        return undefined;
    }
    if (!res.ok) return undefined;
    const ixscan = findWildcardIxscanNode(res.queryPlanner.winningPlan);
    if (!ixscan) return [];
    return (ixscan.multiKeyPaths && ixscan.multiKeyPaths[fieldName]) || [];
}
