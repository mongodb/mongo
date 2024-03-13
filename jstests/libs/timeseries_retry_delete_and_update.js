/**
 * Tests retrying of time-series delete and update operations that are eligible for retryable writes
 * (specifically single deletes and updates).
 *
 * 'setUpCollection' is a function which performs any necessary set up for the collection after it
 * has already been created.
 *
 * 'checkRetriedCommandsCount' is a function which checks whether the retriedStatementsCount
 * statistic has the expected value and returns the amount by which that statistic was expected to
 * increment.
 *
 * 'retriedStatementsCount' is a function which checks whether the retriedStatementsCount statistic
 * has the expected value and returns the amount by which that statistic was expected to increment.
 */
export function runTimeseriesRetryDeleteAndUpdateTest(
    conn, setUpCollection, checkRetriedCommandsCount, checkRetriedStatementsCount) {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';
    const dateTime = ISODate("2021-07-12T16:00:00Z");
    let collCount = 0;

    let retriedCommandsCount = 0;
    let retriedStatementsCount = 0;

    /**
     * Verifies that a timeseries delete or update command supports retryable writes. The arguments
     * to this function are an array of documents to insert initially, a command builder function
     * that returns the command object given the collection to run it on, and a validate function
     * that validates the result after the command has been applied to the collection.
     */
    const runTest = function(
        initialDocs, cmdBuilderFn, validateFn, expectError = false, statementsRetried = 1) {
        const session = conn.startSession({retryWrites: true});
        const testDB = session.getDatabase(jsTestName());

        const coll = testDB.getCollection('timeseries_retry_delete_and_update_' + collCount++);
        coll.drop();
        if (conn.isMongos()) {
            // TODO (SERVER-87625): Use moveCollection instead of createUnsplittableCollection
            // command once moveCollection registers timeseries collections to the sharding catalog.
            assert.commandWorked(testDB.runCommand({
                createUnsplittableCollection: coll.getName(),
                timeseries: {timeField: timeFieldName, metaField: metaFieldName}
            }));
        } else {
            assert.commandWorked(testDB.createCollection(
                coll.getName(),
                {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        }
        setUpCollection(testDB, coll, metaFieldName);

        assert.commandWorked(testDB.runCommand({
            insert: coll.getName(),
            documents: initialDocs,
            lsid: session.getSessionId(),
            txnNumber: NumberLong(0),
        }));

        // For retryable writes, the server uses 'txnNumber' as the key to look up previously
        // executed operations in the session.
        let cmdObj = cmdBuilderFn(coll);
        cmdObj["lsid"] = session.getSessionId();
        cmdObj["txnNumber"] = NumberLong(1);

        if (expectError) {
            assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.InvalidOptions);
            assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.InvalidOptions);
        } else {
            assert.commandWorked(testDB.runCommand(cmdObj),
                                 'Failed to write bucket on first write');
            assert.commandWorked(testDB.runCommand(cmdObj),
                                 'Failed to write bucket on retry write');
        }

        validateFn(coll);

        retriedCommandsCount +=
            checkRetriedCommandsCount(testDB, retriedCommandsCount, statementsRetried);
        retriedStatementsCount +=
            checkRetriedStatementsCount(testDB, retriedStatementsCount, statementsRetried);

        session.endSession();
    };

    const allDocumentsSameBucket = [
        {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 100},
        {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 101},
        {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 102},
    ];
    const allDocumentsDifferentBuckets = [
        {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 100},
        {[timeFieldName]: dateTime, [metaFieldName]: "B", f: 101},
        {[timeFieldName]: dateTime, [metaFieldName]: "C", f: 102},
    ];

    function deleteCmdBuilderFn(coll) {
        return {delete: coll.getName(), deletes: [{q: {}, limit: 1}]};
    }
    function deleteValidateFn(coll) {
        assert.eq(coll.countDocuments({}), 2, "Expected exactly one document to be deleted.");
    }

    (function testPartialBucketDelete() {
        runTest(allDocumentsSameBucket, deleteCmdBuilderFn, deleteValidateFn);
    })();
    (function testFullBucketDelete() {
        runTest(allDocumentsDifferentBuckets, deleteCmdBuilderFn, deleteValidateFn);
    })();

    function updateCmdBuilderFn(coll) {
        return {
            update: coll.getName(),
            updates: [
                {q: {}, u: {$inc: {updated: 1}}, multi: false},
                {q: {}, u: {$inc: {updated: 1}}, multi: true},
                {q: {}, u: {$inc: {anotherUpdated: 1}}, multi: false},
            ],
        };
    }
    function updateCmdUnorderedBuilderFn(coll) {
        let updateCmd = updateCmdBuilderFn(coll);
        updateCmd["ordered"] = false;
        return updateCmd;
    }
    function updateValidateFn(coll) {
        assert.eq(coll.countDocuments({updated: {$exists: true}}),
                  1,
                  "Expected exactly one document to be updated.");
        assert.eq(
            coll.countDocuments({updated: 1}), 1, "Expected document to be updated only once.");
    }
    function updateUnorderedValidateFn(coll) {
        updateValidateFn(coll);
        assert.eq(coll.countDocuments({anotherUpdated: {$exists: true}}),
                  1,
                  "Expected exactly one document to be updated.");
        assert.eq(coll.countDocuments({anotherUpdated: 1}),
                  1,
                  "Expected document to be updated only once.");
    }

    (function testPartialBucketUpdate() {
        runTest(allDocumentsSameBucket,
                updateCmdBuilderFn,
                updateValidateFn,
                /*expectError=*/ true);
    })();
    (function testFullBucketUpdate() {
        runTest(allDocumentsDifferentBuckets,
                updateCmdBuilderFn,
                updateValidateFn,
                /*expectError=*/ true);
    })();
    (function testPartialBucketUpdateUnordered() {
        runTest(allDocumentsSameBucket,
                updateCmdUnorderedBuilderFn,
                updateUnorderedValidateFn,
                /*expectError=*/ true,
                /*statementRetried=*/ 2);
    })();
    (function testFullBucketUpdateUnordered() {
        runTest(allDocumentsDifferentBuckets,
                updateCmdUnorderedBuilderFn,
                updateUnorderedValidateFn,
                /*expectError=*/ true,
                /*statementRetried=*/ 2);
    })();

    function upsertCmdBuilderFn(coll) {
        return {
            update: coll.getName(),
            updates: [{
                q: {[timeFieldName]: dateTime, [metaFieldName]: "B"},
                u: {$inc: {updated: 1}},
                multi: false,
                upsert: true,
            }],
        };
    }
    function upsertValidateFn(coll) {
        assert.eq(coll.countDocuments({[metaFieldName]: "B", updated: 1}),
                  1,
                  "Expected exactly one document to be upserted once.");
    }
    (function testUpsert() {
        runTest(allDocumentsSameBucket, upsertCmdBuilderFn, upsertValidateFn);
    })();
}
