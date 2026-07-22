/**
 * Helpers for primary-driven index build (PDIB) "group atomicity" tests: a batched write concurrent
 * with a PDIB must keep each record's collection write together with its side-table (container)
 * writes in one applyOps entry, even when the batch spans multiple entries.
 */
export const kContainerNss = "admin.$container";

// Returns the applyOps oplog entries for a batched write touching `nss`, sorted by ts. The container
// branch excludes the index build's own periodic `internal-indexBuild-<UUID>` writes, which are
// unrelated to the write under test. When `lsid` is given, restricts to that session's entries. When
// `afterTs` is given, restricts to entries after that timestamp (e.g. to skip seed writes made
// before the write under test).
export function getGroupedApplyOps(node, {nss, lsid, afterTs} = {}) {
    const filter = {
        op: "c",
        "o.applyOps": {
            $elemMatch: {
                $or: [{ns: nss}, {ns: kContainerNss, container: {$not: /^internal-indexBuild-/}}],
            },
        },
    };
    if (lsid) {
        filter["lsid.id"] = lsid;
    }
    if (afterTs) {
        filter.ts = {$gt: afterTs};
    }
    return node.getDB("local").getCollection("oplog.rs").find(filter).sort({ts: 1}).toArray();
}

// Asserts the built index is usable and correct on the secondary by running the same indexed query
// on both nodes and comparing result counts. This is a localized check that the build drained over
// the concurrent writes on the secondary, complementing the broad cross-node consistency that
// stopSet's dbhash/validate provide.
export function assertIndexMatchesOnSecondary(rst, dbName, collName, indexName, query) {
    rst.awaitReplication();
    const secondary = rst.getSecondary();
    secondary.setSecondaryOk();
    const onPrimary = rst.getPrimary().getDB(dbName).getCollection(collName);
    const onSecondary = secondary.getDB(dbName).getCollection(collName);
    const expected = onPrimary.find(query).hint(indexName).itcount();
    assert.gt(expected, 0, "query should match at least one doc via the index on the primary", {
        indexName,
        query,
    });
    assert.eq(
        onSecondary.find(query).hint(indexName).itcount(),
        expected,
        "index returned a different result count on the secondary than the primary",
        {indexName, query},
    );
}

// Returns the non-internal container ops within one applyOps entry's inner op array.
export function containerOpsOf(inners) {
    return inners.filter(
        (op) => op.ns === kContainerNss && !/^internal-indexBuild-/.test(op.container),
    );
}

// Asserts group atomicity: every applyOps entry holding an op on `primaryNss` holds exactly
// `sideWritesPerRecord` container side writes per such op -- so a record's collection write and all
// of its side writes stay in one entry (a torn group would leave the counts out of proportion, e.g.
// a side write in an entry with no collection op). Entries with no `primaryNss` op (e.g. index-build
// drain entries) are skipped. `sideWritesPerRecord` is 1 for a single-index insert, 2 for an update
// that rewrites one key (removed + added), N for N indexes. Invokes `perEntry(entry, inners)` for
// each checked entry, and returns the total {primaryOps, containerOps} counts.
export function assertGroupAtomicity(
    applyOps,
    primaryNss,
    {sideWritesPerRecord = 1, perEntry} = {},
) {
    let primaryOps = 0;
    let containerOps = 0;
    for (const entry of applyOps) {
        const inners = entry.o.applyOps;
        const entryPrimary = inners.filter((op) => op.ns === primaryNss);
        if (entryPrimary.length === 0) {
            continue;
        }
        const entryContainer = containerOpsOf(inners);
        assert.eq(
            entryContainer.length,
            sideWritesPerRecord * entryPrimary.length,
            "expected each record's collection op grouped with all its side writes in one entry",
            {entry, sideWritesPerRecord},
        );
        if (perEntry) {
            perEntry(entry, inners);
        }
        primaryOps += entryPrimary.length;
        containerOps += entryContainer.length;
    }
    return {primaryOps, containerOps};
}
