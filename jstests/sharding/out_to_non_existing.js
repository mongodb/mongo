// Tests for $out with a non-existing target collection.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, config: 1});
    const sourceDB = st.s0.getDB("source_db");

    /**
     * Run an aggregation on 'sourceColl' that writes documents to 'targetColl' with $out.
     */
    function testOut(sourceColl, targetColl, shardedSource) {
        sourceColl.drop();

        if (shardedSource) {
            st.shardColl(sourceColl, {_id: 1}, {_id: 0}, {_id: 1}, sourceDB.getName());
        }

        for (let i = 0; i < 10; i++) {
            assert.commandWorked(sourceColl.insert({_id: i}));
        }

        // Test the behavior for each of the $out modes. Since the target collection does not exist,
        // the behavior should be identical.
        ["insertDocuments", "replaceDocuments", "replaceCollection"].forEach(mode => {
            jsTestLog(
                `Testing $out from ${sourceColl.getFullName()} ` +
                `(${shardedSource ? "sharded" : "unsharded"}) to ${targetColl.getFullName()} ` +
                `with mode "${mode}"`);

            // TODO (SERVER-36832): "replaceCollection" doesn't work in a sharded cluster if the
            // source and target database are different.
            if (mode === "replaceCollection" && sourceColl.getDB() !== targetColl.getDB()) {
                assertErrorCode(sourceColl,
                                [{
                                   $out: {
                                       to: targetColl.getName(),
                                       db: targetColl.getDB().getName(),
                                       mode: "replaceCollection"
                                   }
                                }],
                                50939);
                return;
            }

            targetColl.drop();
            sourceColl.aggregate(
                [{$out: {db: targetColl.getDB().getName(), to: targetColl.getName(), mode: mode}}]);
            assert.eq(10, targetColl.find().itcount());
        });

        // If 'targetColl' is in the same database as 'sourceColl', test the $out stage using the
        // legacy syntax, which must behave identically to mode "replaceCollection".
        if (targetColl.getDB() == sourceColl.getDB()) {
            jsTestLog(
                `Testing $out from ${sourceColl.getFullName()} ` +
                `(${shardedSource ? "sharded" : "unsharded"}) to ${targetColl.getFullName()} ` +
                `with legacy syntax`);

            targetColl.drop();
            sourceColl.aggregate([{$out: targetColl.getName()}]);
            assert.eq(10, targetColl.find().itcount());
        }
    }

    const sourceColl = sourceDB["source_coll"];
    const outputCollSameDb = sourceDB["output_coll"];

    // Test $out from an unsharded source collection to a non-existent output collection in the same
    // database.
    testOut(sourceColl, outputCollSameDb, false);

    // Like the last test case, but perform a $out from a sharded source collection to a
    // non-existent output collection in the same database.
    testOut(sourceColl, outputCollSameDb, true);

    // Test that $out in a sharded cluster fails when the output is sent to a different database
    // that doesn't exist.
    const foreignDb = st.s0.getDB("foreign_db");
    const outputCollDiffDb = foreignDb["output_coll"];
    foreignDb.dropDatabase();
    assert.throws(() => testOut(sourceColl, outputCollDiffDb, false));
    assert.throws(() => testOut(sourceColl, outputCollDiffDb, true));

    // Test $out from an unsharded source collection to an output collection in a different database
    // where the database exists but the collection does not.
    assert.commandWorked(foreignDb["test"].insert({_id: "forcing database creation"}));
    testOut(sourceColl, outputCollDiffDb, false);

    // Like the last test, but with a sharded source collection.
    testOut(sourceColl, outputCollDiffDb, true);
    st.stop();
}());
