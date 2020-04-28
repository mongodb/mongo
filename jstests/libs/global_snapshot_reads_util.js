/**
 * Tests invalid getMore attempts against an established global snapshot cursor on mongos. The
 * cursor should still be valid and usable after each failed attempt.
 */
function verifyInvalidGetMoreAttempts(mainDb, collName, cursorId, lsid, txnNumber) {
    // Reject getMores without a session.
    assert.commandFailedWithCode(
        mainDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}), 50800);

    // Subsequent getMore requests without the same session id are rejected. The cursor should
    // still be valid and usable after this failed attempt.
    assert.commandFailedWithCode(
        mainDb.runCommand(
            {getMore: cursorId, collection: collName, batchSize: 1, lsid: {id: UUID()}}),
        50801);

    // Reject getMores without txnNumber.
    assert.commandFailedWithCode(
        mainDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1, lsid: lsid}),
        50803);

    // Reject getMores without same txnNumber. This fails with NoSuchTransaction because the
    // txnNumber 50 is higher than the active txnNumber for the session.
    assert.commandFailedWithCode(mainDb.runCommand({
        getMore: cursorId,
        collection: collName,
        batchSize: 1,
        lsid: lsid,
        txnNumber: NumberLong(50),
        autocommit: false
    }),
                                 ErrorCodes.NoSuchTransaction);
}

var snapshotReadsCursorTest, snapshotReadsDistinctTest;

(function() {
function makeSnapshotReadConcern(atClusterTime) {
    if (atClusterTime === undefined) {
        return {level: "snapshot"};
    }

    return {level: "snapshot", atClusterTime: atClusterTime};
}

function awaitCommitted(db, ts) {
    jsTestLog(`Wait for ${ts} to be committed on ${db.getMongo()}`);
    assert.soonNoExcept(function() {
        const replSetStatus =
            assert.commandWorked(db.getSiblingDB("admin").runCommand({replSetGetStatus: 1}));
        return timestampCmp(replSetStatus.optimes.readConcernMajorityOpTime.ts, ts) >= 0;
    }, `${ts} was never committed on ${db.getMongo()}`);
}

snapshotReadsCursorTest = function(testScenarioName, primaryDB, secondaryDB, collName) {
    const docs = [...Array(10).keys()].map((i) => ({"_id": i}));

    const commands = {
        aggregate: (batchSize, readPreferenceMode, atClusterTime) => {
            return {
                aggregate: collName,
                pipeline: [{$sort: {_id: 1}}],
                cursor: {batchSize: batchSize},
                readConcern: makeSnapshotReadConcern(atClusterTime),
                $readPreference: {mode: readPreferenceMode}
            };
        },
        find: (batchSize, readPreferenceMode, atClusterTime) => {
            return {
                find: collName,
                sort: {_id: 1},
                batchSize: batchSize,
                readConcern: makeSnapshotReadConcern(atClusterTime),
                $readPreference: {mode: readPreferenceMode}
            };
        }
    };

    for (let useCausalConsistency of [false, true]) {
        for (let [db, readPreferenceMode] of [[primaryDB, "primary"], [secondaryDB, "secondary"]]) {
            jsTestLog(`Running the ${testScenarioName} scenario on collection ` +
                      `${collName} with read preference ${readPreferenceMode} and causal` +
                      ` consistency ${useCausalConsistency}`);

            for (let commandKey in commands) {
                assert(commandKey);
                jsTestLog("Testing the " + commandKey + " command.");
                const command = commands[commandKey];

                let res =
                    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: docs}));
                const insertTimestamp = res.operationTime;
                assert(insertTimestamp);

                jsTestLog(`Inserted 10 documents at timestamp ${insertTimestamp}`);
                awaitCommitted(db, insertTimestamp);

                // Create a session if useCausalConsistency is true.
                let causalDb, sessionTimestamp;

                if (useCausalConsistency) {
                    let session = db.getMongo().startSession({causalConsistency: true});
                    causalDb = session.getDatabase(db.getName());
                    // Establish timestamp.
                    causalDb[collName].findOne({});
                    sessionTimestamp = session.getOperationTime();
                } else {
                    causalDb = db;
                }

                // Establish a snapshot cursor, fetching the first 5 documents.
                res = assert.commandWorked(causalDb.runCommand(command(5, readPreferenceMode)));
                assert.sameMembers(res.cursor.firstBatch, docs.slice(0, 5), res);
                assert(res.cursor.hasOwnProperty("id"));
                const cursorId = res.cursor.id;
                assert.neq(cursorId, 0);
                assert(res.cursor.hasOwnProperty("atClusterTime"));
                let atClusterTime = res.cursor.atClusterTime;
                assert.neq(atClusterTime, Timestamp(0, 0));
                if (useCausalConsistency) {
                    assert.gte(atClusterTime, sessionTimestamp);
                } else {
                    assert.gte(atClusterTime, insertTimestamp);
                }

                // This update is not visible to reads at insertTimestamp.
                res = assert.commandWorked(primaryDB.runCommand(
                    {update: collName, updates: [{q: {}, u: {$set: {x: true}}, multi: true}]}));

                jsTestLog(`Updated collection "${collName}" at timestamp ${res.operationTime}`);
                awaitCommitted(db, res.operationTime);

                // Retrieve the rest of the read command's result set.
                res = assert.commandWorked(
                    causalDb.runCommand({getMore: cursorId, collection: collName}));

                // The cursor has been exhausted. The remaining docs don't show updated field.
                assert.eq(0, res.cursor.id);
                assert.eq(atClusterTime, res.cursor.atClusterTime);
                assert.sameMembers(res.cursor.nextBatch, docs.slice(5), res);

                jsTestLog(`Starting new snapshot read`);

                // This read shows the updated docs.
                res = assert.commandWorked(causalDb.runCommand(command(20, readPreferenceMode)));
                assert.eq(0, res.cursor.id);
                assert(res.cursor.hasOwnProperty("atClusterTime"));
                // Selected atClusterTime at or after first cursor's atClusterTime.
                assert.gte(res.cursor.atClusterTime, atClusterTime);
                assert.sameMembers(res.cursor.firstBatch,
                                   [...Array(10).keys()].map((i) => ({"_id": i, "x": true})),
                                   res);

                jsTestLog(`Reading with original insert timestamp ${insertTimestamp}`);
                // Use non-causal database handle.
                res = assert.commandWorked(
                    db.runCommand(command(20, readPreferenceMode, insertTimestamp)));

                assert.sameMembers(res.cursor.firstBatch, docs, res);
                assert.eq(0, res.cursor.id);
                assert(res.cursor.hasOwnProperty("atClusterTime"));
                assert.eq(res.cursor.atClusterTime, insertTimestamp);

                // Reset.
                assert.commandWorked(
                    primaryDB[collName].remove({}, {writeConcern: {w: "majority"}}));
            }
        }
    }
};

snapshotReadsDistinctTest = function(testScenarioName, primaryDB, secondaryDB, collName) {
    // Note: this test sets documents' "x" field, the test above uses "_id".
    const docs = [...Array(10).keys()].map((i) => ({"x": i}));

    function distinctCommand(readPreferenceMode, atClusterTime) {
        return {
            distinct: collName,
            key: "x",
            readConcern: makeSnapshotReadConcern(atClusterTime),
            $readPreference: {mode: readPreferenceMode}
        };
    }

    for (let useCausalConsistency of [false, true]) {
        for (let [db, readPreferenceMode] of [[primaryDB, "primary"], [secondaryDB, "secondary"]]) {
            jsTestLog(`Testing "distinct" on collection ` +
                      `${collName} with read preference ${readPreferenceMode} and causal` +
                      ` consistency ${useCausalConsistency}`);

            let res =
                assert.commandWorked(primaryDB.runCommand({insert: collName, documents: docs}));
            const insertTimestamp = res.operationTime;
            assert(insertTimestamp);

            jsTestLog(`Inserted 10 documents at timestamp ${insertTimestamp}`);
            awaitCommitted(db, insertTimestamp);

            // Create a session if useCausalConsistency is true.
            let causalDb, sessionTimestamp;

            if (useCausalConsistency) {
                let session = db.getMongo().startSession({causalConsistency: true});
                causalDb = session.getDatabase(db.getName());
                // Establish timestamp.
                causalDb[collName].findOne({});
                sessionTimestamp = session.getOperationTime();
            } else {
                causalDb = db;
            }

            // Execute "distinct".
            res = assert.commandWorked(causalDb.runCommand(distinctCommand(readPreferenceMode)));
            const xs = [...Array(10).keys()];
            assert.sameMembers(xs, res.values);
            assert(res.hasOwnProperty("atClusterTime"));
            let atClusterTime = res.atClusterTime;
            assert.neq(atClusterTime, Timestamp(0, 0));
            if (useCausalConsistency) {
                assert.gte(atClusterTime, sessionTimestamp);
            } else {
                assert.gte(atClusterTime, insertTimestamp);
            }

            // Set all "x" fields to 42. This update is not visible to reads at insertTimestamp.
            res = assert.commandWorked(primaryDB.runCommand(
                {update: collName, updates: [{q: {}, u: {$set: {x: 42}}, multi: true}]}));

            jsTestLog(`Updated collection "${collName}" at timestamp ${res.operationTime}`);
            awaitCommitted(db, res.operationTime);

            // This read shows the updated docs.
            res = assert.commandWorked(causalDb.runCommand(distinctCommand(readPreferenceMode)));
            assert(res.hasOwnProperty("atClusterTime"));
            // Selected atClusterTime at or after first read's atClusterTime.
            assert.gte(res.atClusterTime, atClusterTime);
            assert.sameMembers([42], res.values);

            jsTestLog(`Reading with original insert timestamp ${insertTimestamp}`);
            // Use non-causal database handle.
            res = assert.commandWorked(
                db.runCommand(distinctCommand(readPreferenceMode, insertTimestamp)));

            assert.sameMembers(xs, res.values);
            assert(res.hasOwnProperty("atClusterTime"));
            assert.eq(res.atClusterTime, insertTimestamp);

            // Reset.
            assert.commandWorked(primaryDB[collName].remove({}, {writeConcern: {w: "majority"}}));
        }
    }
};
})();
