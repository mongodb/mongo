export const numMostCommonValues = 5;

/**
 * If 'expectEntries' is true , asserts that there are profiler entries for aggregate commands for
 * the analyzeShardKey command with the given 'comment', and verifies that the aggregate commands
 * used index scan and fetch no more 'numMostCommonValues' documents. If 'expectEntries' is false,
 * there are such profiler entries.
 */
export function assertAggregateQueryPlans(mongodConns, dbName, collName, comment, expectEntries) {
    let numEntries = 0;

    mongodConns.forEach(conn => {
        const profilerColl = conn.getDB(dbName).system.profile;

        profilerColl.find({"command.aggregate": collName, "command.comment": comment})
            .forEach(doc => {
                if (doc.hasOwnProperty("ok") && (doc.ok === 0)) {
                    return;
                }

                const firstStage = doc.command.pipeline[0];

                if (firstStage.hasOwnProperty("$collStats")) {
                    return;
                }

                numEntries++;
                if (firstStage.hasOwnProperty("$match") || firstStage.hasOwnProperty("$limit")) {
                    // This corresponds to the aggregation that the analyzeShardKey command runs
                    // to look up documents for a shard key with a unique or hashed supporting
                    // index. For both cases, it should fetch at most 'numMostCommonValues'
                    // documents.
                    assert(doc.hasOwnProperty("planSummary"), doc);
                    assert.lte(doc.docsExamined, numMostCommonValues, doc);
                } else {
                    // This corresponds to the aggregation that the analyzeShardKey command runs
                    // when analyzing a shard key with a non-unique supporting index.
                    if (!firstStage.hasOwnProperty("$mergeCursors")) {
                        assert(doc.hasOwnProperty("planSummary"), doc);
                        assert(doc.planSummary.includes("IXSCAN"), doc);
                    }

                    // Verify that it fetched at most 'numMostCommonValues' documents.
                    assert.lte(doc.docsExamined, numMostCommonValues, doc);
                    // Verify that it opted out of shard filtering.
                    assert.eq(doc.readConcern.level, "available", doc);
                }
            });
    });

    if (expectEntries) {
        assert.gt(numEntries, 0);
    } else {
        assert.eq(numEntries, 0);
    }
}

/**
 * Returns the connections to all data-bearing mongods in the sharded cluster or replica set.
 */
export function getMongodConns({st, rst}) {
    assert(st || rst);
    assert(!st || !rst);
    const conns = [];
    if (st) {
        st._rs.forEach((rst) => {
            rst.nodes.forEach(node => conns.push(node));
        });
    } else {
        rst.nodes.forEach(node => conns.push(node));
    }
    return conns;
}
