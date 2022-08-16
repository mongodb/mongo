/**
 * Test which verifies that $out/$merge aggregations with secondary read preference which write
 * over 16 MB work as expected (especially with respect to producing correctly sized write batches).
 *
 * @tags: [uses_$out, assumes_read_preference_unchanged]
 */
(function() {
const dbName = "db";
const collName = "movies";
const targetCollName = "movies2";

function testFn(db) {
    const coll = db[collName];
    coll.drop();
    db[targetCollName].drop();

    // Insert 4 MB more than the maximum bytes allowed in a single write batch worth of data
    // serialized as a single BSONObj.
    const hello = db.hello();
    const maxBatchSize = hello.maxWriteBatchSize;
    const totalDataSize = hello.maxBsonObjectSize + (4 * 1024 * 1024);
    const sizePerDoc = totalDataSize / maxBatchSize;
    const bigString = "a".repeat(sizePerDoc);
    const bulk = coll.initializeUnorderedBulkOp();

    for (let i = 0; i < maxBatchSize; ++i) {
        bulk.insert({_id: NumberInt(i), foo: bigString});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    function defaultSetUpFn(db) {
        db[targetCollName].drop({writeConcern: {w: "majority"}});
    }

    function cleanUpFn(db) {
        db[targetCollName].drop({writeConcern: {w: "majority"}});
    }

    function testWriteAggSpec(aggWriteStageSpec, setUpFn, errorCodes = []) {
        // Run 'aggWriteStageSpec' with both primary and secondary read preference.
        for (const readPref of ["primary", "secondary"]) {
            jsTestLog("Testing " + tojson(aggWriteStageSpec) + " with read preference " + readPref);
            setUpFn(db);

            // If the caller provided some error codes, assert that the command failed with one
            // of these codes.
            const fn = () =>
                db[collName]
                    .aggregate([aggWriteStageSpec], {$readPreference: {mode: readPref}})
                    .itcount();
            const errMsg = "Failed to run aggregate with read preference " + readPref;
            if (errorCodes.length > 0) {
                assert.throwsWithCode(fn, errorCodes, [] /* params */, errMsg);
            } else {
                assert.doesNotThrow(fn, [] /* params */, errMsg);
            }
            cleanUpFn(db);
        }
    }

    // Set up documents in the output collection so that $merge will perform updates.
    function mergeUpdateSetupFn(db) {
        defaultSetUpFn(db);
        const bulk = db[targetCollName].initializeUnorderedBulkOp();
        for (let i = 0; i < maxBatchSize; ++i) {
            bulk.insert({_id: NumberInt(i), extraField: i * 3});
        }
        assert.commandWorked(bulk.execute({w: "majority"}));
    }

    testWriteAggSpec({$out: targetCollName}, defaultSetUpFn);
    testWriteAggSpec(
        {$merge: {into: targetCollName, whenMatched: "replace", whenNotMatched: "insert"}},
        defaultSetUpFn);
    testWriteAggSpec(
        {$merge: {into: targetCollName, whenMatched: "merge", whenNotMatched: "insert"}},
        defaultSetUpFn);
    testWriteAggSpec(
        {$merge: {into: targetCollName, whenMatched: "keepExisting", whenNotMatched: "insert"}},
        defaultSetUpFn);
    testWriteAggSpec(
        {$merge: {into: targetCollName, whenMatched: "merge", whenNotMatched: "insert", on: "_id"}},
        mergeUpdateSetupFn);

    // Failure cases.
    testWriteAggSpec(
        {$merge: {into: targetCollName, whenMatched: "replace", whenNotMatched: "fail", on: "_id"}},
        defaultSetUpFn,
        [ErrorCodes.MergeStageNoMatchingDocument]);
    testWriteAggSpec(
        {$merge: {into: targetCollName, whenMatched: "merge", whenNotMatched: "fail", on: "_id"}},
        defaultSetUpFn,
        [ErrorCodes.MergeStageNoMatchingDocument]);
    testWriteAggSpec(
        {$merge: {into: targetCollName, whenMatched: "fail", whenNotMatched: "insert", on: "_id"}},
        mergeUpdateSetupFn,
        [ErrorCodes.DuplicateKey]);
}

jsTestLog("Testing against a replica set");
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
testFn(new Mongo(rst.getURL()).getDB(dbName));
rst.stopSet();

jsTestLog("Testing against a sharded cluster");
const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
testFn(st.s.getDB(dbName));
st.stop();
}());
