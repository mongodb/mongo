/**
 * Test which verifies that $out/$merge aggregations with secondary read preference which write
 * over 16 MB work as expected (especially with respect to producing correctly sized write batches).
 */

/**
 * Available test cases:
 *   - "out": Tests $out stage
 *   - "merge_replace_insert": Tests $merge with whenMatched: "replace", whenNotMatched: "insert"
 *   - "merge_merge_insert": Tests $merge with whenMatched: "merge", whenNotMatched: "insert"
 *   - "merge_keep_existing": Tests $merge with whenMatched: "keepExisting", whenNotMatched: "insert"
 *   - "merge_update": Tests $merge with update setup (whenMatched: "merge", on: "_id")
 *   - "merge_replace_fail": Tests $merge with whenMatched: "replace", whenNotMatched: "fail" (failure case)
 *   - "merge_merge_fail": Tests $merge with whenMatched: "merge", whenNotMatched: "fail" (failure case)
 *   - "merge_fail_insert": Tests $merge with whenMatched: "fail", whenNotMatched: "insert" (failure case)
 *
 * @param {Object} db - The database object to use
 * @param {Function} awaitReplication - Function to call to await replication
 * @param {string} [testCase] - Optional specific test case to run. If not provided, runs all tests.
 */
export function testOutAndMergeOnSecondaryBatchWrite(db, awaitReplication, testCase = null) {
    const collName = "movies";
    const targetCollName = "movies2";

    const coll = db[collName];
    coll.drop();
    db[targetCollName].drop();

    // Insert 4 MB more than the maximum bytes allowed in a single write batch worth of data
    // serialized as a single BSONObj.
    const hello = db.hello();
    const maxBatchSize = hello.maxWriteBatchSize;
    const totalDataSize = hello.maxBsonObjectSize + 4 * 1024 * 1024;
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
            awaitReplication();

            // If the caller provided some error codes, assert that the command failed with one
            // of these codes.
            const fn = () => db[collName].aggregate([aggWriteStageSpec], {$readPreference: {mode: readPref}}).itcount();
            const errMsg = "Failed to run aggregate with read preference " + readPref;
            if (errorCodes.length > 0) {
                assert.throwsWithCode(fn, errorCodes, [] /* params */, errMsg);
            } else {
                assert.doesNotThrow(fn, [] /* params */, errMsg);
            }
            cleanUpFn(db);
            awaitReplication();
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

    // Define all test cases
    const testCases = {
        "out": () => testWriteAggSpec({$out: targetCollName}, defaultSetUpFn),
        "merge_replace_insert": () =>
            testWriteAggSpec(
                {$merge: {into: targetCollName, whenMatched: "replace", whenNotMatched: "insert"}},
                defaultSetUpFn,
            ),
        "merge_merge_insert": () =>
            testWriteAggSpec(
                {$merge: {into: targetCollName, whenMatched: "merge", whenNotMatched: "insert"}},
                defaultSetUpFn,
            ),
        "merge_keep_existing": () =>
            testWriteAggSpec(
                {$merge: {into: targetCollName, whenMatched: "keepExisting", whenNotMatched: "insert"}},
                defaultSetUpFn,
            ),
        "merge_update": () =>
            testWriteAggSpec(
                {$merge: {into: targetCollName, whenMatched: "merge", whenNotMatched: "insert", on: "_id"}},
                mergeUpdateSetupFn,
            ),
        "merge_replace_fail": () =>
            testWriteAggSpec(
                {$merge: {into: targetCollName, whenMatched: "replace", whenNotMatched: "fail", on: "_id"}},
                defaultSetUpFn,
                [ErrorCodes.MergeStageNoMatchingDocument],
            ),
        "merge_merge_fail": () =>
            testWriteAggSpec(
                {$merge: {into: targetCollName, whenMatched: "merge", whenNotMatched: "fail", on: "_id"}},
                defaultSetUpFn,
                [ErrorCodes.MergeStageNoMatchingDocument],
            ),
        "merge_fail_insert": () =>
            testWriteAggSpec(
                {$merge: {into: targetCollName, whenMatched: "fail", whenNotMatched: "insert", on: "_id"}},
                mergeUpdateSetupFn,
                [ErrorCodes.DuplicateKey],
            ),
    };

    // Run either the specified test case or all test cases
    if (testCase !== null) {
        if (!(testCase in testCases)) {
            throw new Error(
                `Unknown test case: ${testCase}. Valid test cases are: ${Object.keys(testCases).join(", ")}`,
            );
        }
        jsTestLog(`Running single test case: ${testCase}`);
        testCases[testCase]();
    } else {
        // Run all test cases for backwards compatibility
        for (const [name, testFn] of Object.entries(testCases)) {
            jsTestLog(`Running test case: ${name}`);
            testFn();
        }
    }
}
