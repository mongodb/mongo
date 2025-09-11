/*
 * Helpers for basic testing of replicated idents.
 */
function getOplog(node) {
    return node.getDB("local").oplog.rs;
}

export function getSortedCatalogEntries(node, sortField = "ident") {
    const adminDB = node.getDB("admin");
    const isSystemProfile = {"name": "system.profile"};
    const isLocal = {"db": "local"};
    const match = {$nor: [isSystemProfile, isLocal]};
    return adminDB.aggregate([{$listCatalog: {}}, {$match: match}, {$sort: {sortField: 1}}]).toArray();
}
/**
 * Given catalog entries for 2 nodes, where catalog entries for both nodes must be sorted by the
 * same field, validates that each entry has a matching 'ident'.
 */
export function assertMatchingCatalogIdents(node0CatalogIdents, node1CatalogIdents) {
    jsTest.log(
        `Asserting catalog entries for node0 ${tojson(node0CatalogIdents)} with node1 ${tojson(node1CatalogIdents)}`,
    );
    assert.eq(
        node0CatalogIdents.length,
        node1CatalogIdents.length,
        `Expected nodes to have same number of entries. Entries for node0 ${tojson(
            node0CatalogIdents,
        )}, entries for node1 ${node1CatalogIdents}`,
    );

    const numCatalogEntries = node0CatalogIdents.length;
    const entriesThatDontMatch = [];
    for (let i = 0; i < numCatalogEntries; i++) {
        const entryNode0 = node0CatalogIdents[i];
        const entryNode1 = node1CatalogIdents[i];

        if (bsonWoCompare(entryNode0, entryNode1) !== 0) {
            // For visibility, collect all mismatched entries before failing.
            entriesThatDontMatch.push([entryNode0, entryNode1]);
            jsTest.log(
                `Expected both nodes to have same entries. Node0 has ${tojson(
                    entryNode0,
                )}, Node1 has ${tojson(entryNode1)}`,
            );
        }
    }

    assert.eq(
        0,
        entriesThatDontMatch.length,
        `Catalog entries for were expected to match, but don't. Entries that don't match ${tojson(
            entriesThatDontMatch,
        )}`,
    );
}

// Validates that all 'create' collection oplog entries contain collection idents.
export function assertCreateOplogEntriesContainIdents(node) {
    const createOps = getOplog(node)
        .find({"op": "c", "o.create": {$exists: true}})
        .toArray();
    jsTest.log("Create oplog entries on node " + node.port + " " + tojson(createOps));
    assert.lt(0, createOps.length);
    for (let op of createOps) {
        assert(
            op.hasOwnProperty("o2"),
            `Expected to have 'o2' field present in ${tojson(
                op,
            )}. Dumping all create oplog entries ${tojson(createOps)}`,
        );

        const o2 = op["o2"];
        assert(
            o2.hasOwnProperty("ident"),
            `Expected to find 'ident' property in 'o2' field of ${tojson(
                op,
            )}. Dumping all create oplog entries ${tojson(createOps)}`,
        );
        assert(
            o2.hasOwnProperty("idIndexIdent"),
            `Expected to find 'idIndexIdent' property in 'o2' field of ${tojson(
                op,
            )}. Dumping all create oplog entries ${tojson(createOps)}`,
        );
    }
}
