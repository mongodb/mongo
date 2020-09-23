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

/**
 * Test non-transaction snapshot reads on primary and secondary.
 *
 * Pass two handles to the same database; either both connected to a mongos, or one connected to
 * a replica set primary and the other connected to a replica set secondary. (The test will also
 * pass $readPreference, so if the handles are connected to a mongos, then the reads will target
 * primary/secondary shard servers.)
 *
 * For awaitCommittedFn, pass a function that waits for the last write to be committed on all
 * secondaries.
 *
 * @param {primaryDB} Database handle connected to a primary or mongos
 * @param {secondaryDB} Database handle connected to a secondary or mongos
 * @param {collName} String
 * @param {awaitCommittedFn} A function with no arguments or return value
 */
function SnapshotReadsTest({primaryDB, secondaryDB, awaitCommittedFn}) {
    function _makeSnapshotReadConcern(atClusterTime) {
        if (atClusterTime === undefined) {
            return {level: "snapshot"};
        }

        return {level: "snapshot", atClusterTime: atClusterTime};
    }

    /**
     * Test non-transaction snapshot "find" and "aggregate".
     *
     * @param {testScenarioName} String used when logging progress
     * @param {collName} String
     */
    this.cursorTest = function({testScenarioName, collName}) {
        const docs = [...Array(10).keys()].map((i) => ({"_id": i}));

        const commands = {
            aggregate: (batchSize, readPreferenceMode, atClusterTime) => {
                return {
                    aggregate: collName,
                    pipeline: [{$sort: {_id: 1}}],
                    cursor: {batchSize: batchSize},
                    readConcern: _makeSnapshotReadConcern(atClusterTime),
                    $readPreference: {mode: readPreferenceMode}
                };
            },
            find: (batchSize, readPreferenceMode, atClusterTime) => {
                return {
                    find: collName,
                    sort: {_id: 1},
                    batchSize: batchSize,
                    readConcern: _makeSnapshotReadConcern(atClusterTime),
                    $readPreference: {mode: readPreferenceMode}
                };
            }
        };

        for (let useCausalConsistency of [false, true]) {
            for (let [db, readPreferenceMode] of [[primaryDB, "primary"],
                                                  [secondaryDB, "secondary"]]) {
                jsTestLog(`Running the ${testScenarioName} scenario on collection ` +
                          `${collName} with read preference ${readPreferenceMode} and causal` +
                          ` consistency ${useCausalConsistency}`);

                for (let commandKey in commands) {
                    assert(commandKey);
                    jsTestLog("Testing the " + commandKey + " command.");
                    const command = commands[commandKey];

                    let res = assert.commandWorked(
                        primaryDB.runCommand({insert: collName, documents: docs}));
                    const insertTimestamp = res.operationTime;
                    assert(insertTimestamp);

                    jsTestLog(`Inserted 10 documents at timestamp ${tojson(insertTimestamp)}`);
                    awaitCommittedFn(db, insertTimestamp);

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
                    jsTestLog(`Updated collection "${collName}" at timestamp ${
                        tojson(res.operationTime)}`);

                    awaitCommittedFn(db, res.operationTime);

                    // This index is not visible to reads at insertTimestamp and does not cause the
                    // operation to fail.
                    res = assert.commandWorked(primaryDB.runCommand(
                        {createIndexes: collName, indexes: [{key: {x: 1}, name: 'x_1'}]}));
                    jsTestLog(`Created an index on collection "${collName}" at timestamp ${
                        tojson(res.operationTime)}`);
                    awaitCommittedFn(db, res.operationTime);

                    // Retrieve the rest of the read command's result set.
                    res = assert.commandWorked(
                        causalDb.runCommand({getMore: cursorId, collection: collName}));

                    // The cursor has been exhausted. The remaining docs don't show updated field.
                    assert.eq(0, res.cursor.id);
                    assert.eq(atClusterTime, res.cursor.atClusterTime);
                    assert.sameMembers(res.cursor.nextBatch, docs.slice(5), res);

                    jsTestLog(`Starting new snapshot read`);

                    // This read shows the updated docs.
                    res =
                        assert.commandWorked(causalDb.runCommand(command(20, readPreferenceMode)));
                    assert.eq(0, res.cursor.id);
                    assert(res.cursor.hasOwnProperty("atClusterTime"));
                    // Selected atClusterTime at or after first cursor's atClusterTime.
                    assert.gte(res.cursor.atClusterTime, atClusterTime);
                    assert.sameMembers(res.cursor.firstBatch,
                                       [...Array(10).keys()].map((i) => ({"_id": i, "x": true})),
                                       res);

                    jsTestLog(`Reading with original insert timestamp ${tojson(insertTimestamp)}`);
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

    /**
     * Test non-transaction snapshot "distinct" on primary and secondary.
     *
     * @param {testScenarioName} String used when logging progress
     * @param {collName} String
     */
    this.distinctTest = function({testScenarioName, collName}) {
        // Note: this test sets documents' "x" field, whereas cursorTest uses "_id".
        const docs = [...Array(10).keys()].map((i) => ({"x": i}));

        function distinctCommand(readPreferenceMode, atClusterTime) {
            return {
                distinct: collName,
                key: "x",
                readConcern: _makeSnapshotReadConcern(atClusterTime),
                $readPreference: {mode: readPreferenceMode}
            };
        }

        for (let useCausalConsistency of [false, true]) {
            for (let [db, readPreferenceMode] of [[primaryDB, "primary"],
                                                  [secondaryDB, "secondary"]]) {
                jsTestLog(`Testing "distinct" with the ${testScenarioName} scenario on` +
                          ` collection ${collName} with read preference ${
                              readPreferenceMode} and causal` +
                          ` consistency ${useCausalConsistency}`);

                let res =
                    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: docs}));
                const insertTimestamp = res.operationTime;
                assert(insertTimestamp);

                jsTestLog(`Inserted 10 documents at timestamp ${tojson(insertTimestamp)}`);
                awaitCommittedFn(db, insertTimestamp);

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
                res =
                    assert.commandWorked(causalDb.runCommand(distinctCommand(readPreferenceMode)));
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

                jsTestLog(
                    `Updated collection "${collName}" at timestamp ${tojson(res.operationTime)}`);
                awaitCommittedFn(db, res.operationTime);

                // This read shows the updated docs.
                res =
                    assert.commandWorked(causalDb.runCommand(distinctCommand(readPreferenceMode)));
                assert(res.hasOwnProperty("atClusterTime"));
                // Selected atClusterTime at or after first read's atClusterTime.
                assert.gte(res.atClusterTime, atClusterTime);
                assert.sameMembers([42], res.values);

                jsTestLog(`Reading with original insert timestamp ${tojson(insertTimestamp)}`);
                // Use non-causal database handle.
                res = assert.commandWorked(
                    db.runCommand(distinctCommand(readPreferenceMode, insertTimestamp)));

                assert.sameMembers(xs, res.values);
                assert(res.hasOwnProperty("atClusterTime"));
                assert.eq(res.atClusterTime, insertTimestamp);

                // Reset.
                assert.commandWorked(
                    primaryDB[collName].remove({}, {writeConcern: {w: "majority"}}));
            }
        }
    };

    this.lookupAndUnionWithTest = function({testScenarioName, coll1, coll2, isColl2Sharded}) {
        const docs = [...Array(10).keys()].map((i) => ({_id: i, x: i}));
        const lookupExpected =
            [...Array(10).keys()].map((i) => ({_id: i, x: i, y: [{_id: i, x: i}]}));
        const unionWithExpected = [...Array(10).keys()].reduce((acc, i) => {
            return acc.concat([{_id: i, x: i}, {_id: i, x: i}]);
        }, []);

        for (let [db, readPreferenceMode] of [[primaryDB, "primary"], [secondaryDB, "secondary"]]) {
            jsTestLog(
                `Testing "$lookup" and "$unionWith" with the ${testScenarioName} scenario ` +
                `on collections ${coll1} and ${coll2} with read preference ${readPreferenceMode} `);

            let res;
            assert.commandWorked(primaryDB.runCommand({insert: coll1, documents: docs}));
            res = assert.commandWorked(primaryDB.runCommand({insert: coll2, documents: docs}));
            const atClusterTimeReadConcern = {level: "snapshot", atClusterTime: res.operationTime};
            jsTestLog(`Inserted 10 documents on each collection at timestamp ${
                tojson(res.operationTime)}`);
            awaitCommittedFn(db, res.operationTime);

            const lookup = (readConcern) => {
                return {
                    aggregate: coll1,
                    pipeline: [
                        {$lookup: {from: coll2, localField: "x", foreignField: "x", as: "y"}},
                        {$sort: {_id: 1}}
                    ],
                    cursor: {},
                    readConcern: readConcern,
                    $readPreference: {mode: readPreferenceMode}
                };
            };

            const unionWith = (readConcern) => {
                return {
                    aggregate: coll1,
                    pipeline: [{$unionWith: coll2}, {$sort: {_id: 1}}],
                    cursor: {},
                    readConcern: readConcern,
                    $readPreference: {mode: readPreferenceMode}
                };
            };

            let lookupSnapshot;
            // The "from" collection cannot be sharded for $lookup.
            if (!isColl2Sharded) {
                jsTestLog("Test aggregate $lookup with snapshot");
                lookupSnapshot = assert.commandWorked(db.runCommand(lookup({level: "snapshot"})))
                                     .cursor.firstBatch;
                assert.eq(lookupExpected, lookupSnapshot, () => {
                    return "Expected lookup results: " + tojson(lookupExpected) +
                        " Got: " + tojson(lookupSnapshot);
                });
            }

            jsTestLog("Test aggregate $unionWith with snapshot");
            const unionWithSnapshot =
                assert.commandWorked(db.runCommand(unionWith({level: "snapshot"})))
                    .cursor.firstBatch;
            assert.eq(unionWithExpected, unionWithSnapshot, () => {
                return "Expected unionWith results: " + tojson(unionWithExpected) +
                    " Got: " + tojson(unionWithSnapshot);
            });

            assert.commandWorked(primaryDB.runCommand(
                {update: coll1, updates: [{q: {}, u: {$inc: {x: 10}}, multi: true}]}));
            res = assert.commandWorked(primaryDB.runCommand(
                {update: coll2, updates: [{q: {}, u: {$inc: {x: 10}}, multi: true}]}));
            jsTestLog(`Updated both collections at timestamp ${tojson(res.operationTime)}`);
            awaitCommittedFn(db, res.operationTime);

            // The "from" collection cannot be sharded for $lookup.
            if (!isColl2Sharded) {
                jsTestLog("Test aggregate $lookup with atClusterTime");
                const lookupAtClusterTime =
                    assert.commandWorked(db.runCommand(lookup(atClusterTimeReadConcern)))
                        .cursor.firstBatch;
                assert.eq(lookupExpected, lookupAtClusterTime, () => {
                    return "Expected lookup results: " + tojson(lookupExpected) +
                        " Got: " + tojson(lookupAtClusterTime);
                });
            }

            jsTestLog("Test aggregate $unionWith with atClusterTime");
            const unionWithAtClusterTime =
                assert.commandWorked(db.runCommand(unionWith(atClusterTimeReadConcern)))
                    .cursor.firstBatch;
            assert.eq(unionWithExpected, unionWithAtClusterTime, () => {
                return "Expected unionWith results: " + tojson(unionWithExpected) +
                    " Got: " + tojson(unionWithAtClusterTime);
            });

            // Reset for the next run.
            assert.commandWorked(primaryDB[coll1].remove({}, {writeConcern: {w: "majority"}}));
            assert.commandWorked(primaryDB[coll2].remove({}, {writeConcern: {w: "majority"}}));
        }
    };

    this.outAndMergeTest = function({testScenarioName, coll, outColl, isOutCollSharded}) {
        const docs = [...Array(10).keys()].map((i) => ({_id: i, x: i}));

        for (let [db, readPreferenceMode] of [[primaryDB, "primary"], [secondaryDB, "secondary"]]) {
            jsTestLog(`Testing "$out" and "$merge" with the ${testScenarioName} scenario on` +
                      ` collection ${coll} with read preference ${readPreferenceMode} ` +
                      `and output collection ${outColl}`);

            let res = assert.commandWorked(primaryDB.runCommand({insert: coll, documents: docs}));
            const atClusterTimeReadConcern = {level: "snapshot", atClusterTime: res.operationTime};
            jsTestLog(`Inserted 10 documents at timestamp ${tojson(res.operationTime)}`);
            awaitCommittedFn(db, res.operationTime);

            const out = (readConcern) => {
                return {
                    aggregate: coll,
                    pipeline: [{$out: outColl}],
                    cursor: {},
                    readConcern: readConcern,
                    $readPreference: {mode: readPreferenceMode}
                };
            };

            const merge = (readConcern) => {
                return {
                    aggregate: coll,
                    pipeline: [{$merge: outColl}],
                    cursor: {},
                    readConcern: readConcern,
                    $readPreference: {mode: readPreferenceMode}
                };
            };

            // The "out" collection cannot be sharded for $out.
            if (!isOutCollSharded) {
                jsTestLog("Test aggregate $out with snapshot");
                assert.commandWorked(
                    primaryDB[outColl].remove({}, {writeConcern: {w: "majority"}}));
                res = assert.commandWorked(db.runCommand(out({level: "snapshot"})));
                awaitCommittedFn(db, res.operationTime);
                res = db[outColl].find().sort({_id: 1}).toArray();
                assert.eq(docs, res, () => {
                    return "Expected out results: " + tojson(docs) + " Got: " + tojson(res);
                });
            }

            jsTestLog("Test aggregate $merge with snapshot");
            assert.commandWorked(primaryDB[outColl].remove({}, {writeConcern: {w: "majority"}}));
            res = assert.commandWorked(db.runCommand(merge({level: "snapshot"})));
            awaitCommittedFn(db, res.operationTime);
            res = db[outColl].find().sort({_id: 1}).toArray();
            assert.eq(docs, res, () => {
                return "Expected merge results: " + tojson(docs) + " Got: " + tojson(res);
            });

            res = assert.commandWorked(primaryDB.runCommand(
                {update: coll, updates: [{q: {}, u: {$inc: {x: 10}}, multi: true}]}));
            jsTestLog(`Updated collection "${coll}" at timestamp ${tojson(res.operationTime)}`);
            awaitCommittedFn(db, res.operationTime);

            // The "out" collection cannot be sharded for $out.
            if (!isOutCollSharded) {
                jsTestLog("Test aggregate $out with atClusterTime");
                assert.commandWorked(
                    primaryDB[outColl].remove({}, {writeConcern: {w: "majority"}}));
                res = assert.commandWorked(db.runCommand(out(atClusterTimeReadConcern)));
                awaitCommittedFn(db, res.operationTime);
                res = db[outColl].find().sort({_id: 1}).toArray();
                assert.eq(docs, res, () => {
                    return "Expected out results: " + tojson(docs) + " Got: " + tojson(res);
                });
            }

            jsTestLog("Test aggregate $merge with atClusterTime");
            assert.commandWorked(primaryDB[outColl].remove({}, {writeConcern: {w: "majority"}}));
            res = assert.commandWorked(db.runCommand(merge(atClusterTimeReadConcern)));
            awaitCommittedFn(db, res.operationTime);
            res = db[outColl].find().sort({_id: 1}).toArray();
            assert.eq(docs, res, () => {
                return "Expected merge results: " + tojson(docs) + " Got: " + tojson(res);
            });

            // Reset for the next run.
            assert.commandWorked(primaryDB[coll].remove({}, {writeConcern: {w: "majority"}}));
            assert.commandWorked(primaryDB[outColl].remove({}, {writeConcern: {w: "majority"}}));
        }
    };
}
