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

function snapshotReadsTest(testScenarioName, db, collName) {
    const docs = [...Array(10).keys()].map((i) => ({"_id": i}));

    function makeSnapshotReadConcern(atClusterTime) {
        if (atClusterTime === undefined) {
            return {level: "snapshot"};
        }

        return {level: "snapshot", atClusterTime: atClusterTime};
    }

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
        for (let readPreferenceMode of ["primary", "secondary"]) {
            jsTestLog(`Running the ${testScenarioName} scenario on collection ` +
                      `${collName} with read preference ${readPreferenceMode} and causal` +
                      ` consistency ${useCausalConsistency}`);

            for (let commandKey in commands) {
                assert(commandKey);
                jsTestLog("Testing the " + commandKey + " command.");
                const command = commands[commandKey];

                let res = assert.commandWorked(db.runCommand(
                    {insert: collName, documents: docs, writeConcern: {w: "majority"}}));
                const insertTimestamp = res.operationTime;
                assert(insertTimestamp);

                jsTestLog(`Inserted 10 documents at timestamp ${insertTimestamp}`);

                // Create a session if useCausalConsistency is true.
                let causalDb, sessionTimestamp;

                if (useCausalConsistency) {
                    let session = db.getMongo().startSession({causalConsistency: true});
                    causalDb = session.getDatabase(db.getName());
                    // Establish timestamp.
                    causalDb["otherCollection"].insertOne({});
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
                res = assert.commandWorked(causalDb.runCommand({
                    update: collName,
                    updates: [{q: {}, u: {$set: {x: true}}, multi: true}],
                    writeConcern: {w: "majority"}
                }));

                jsTestLog(`Updated collection "${collName}" at timestamp ${res.operationTime}`);

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
                assert.commandWorked(db[collName].remove({}, {writeConcern: {w: "majority"}}));
            }
        }
    }
}
