/**
 * Test that write errors in transactions are reported in the writeErrors array, except for
 * TransientTransactionErrors.
 * @tags: [requires_capped, uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const testDB = db.getSiblingDB(dbName);
    const testCollName = "transactions_write_errors";
    const cappedCollName = "capped_transactions_write_errors";
    const testColl = testDB[testCollName];
    const cappedColl = testDB[cappedCollName];

    testDB.runCommand({drop: testCollName, writeConcern: {w: "majority"}});
    testDB.runCommand({drop: cappedCollName, writeConcern: {w: "majority"}});

    assert.commandWorked(testDB.createCollection(testColl.getName()));
    assert.commandWorked(testDB.createCollection(cappedCollName, {capped: true, size: 1000}));

    // Assert that "cmd" fails with error "code" after "nExpected" operations, or fail with "msg"
    function runInTxn({cmd, msg, code, nExpected, expectedErrorIndex}) {
        const session = db.getMongo().startSession();
        session.startTransaction();
        try {
            var res = session.getDatabase(dbName).runCommand(cmd);
            try {
                // Writes reply with ok: 1 and a writeErrors array
                assert.eq(res.ok, 1, "reply.ok : " + msg);
                assert.eq(res.n, nExpected, "reply.n : " + msg);
                // The first and only error comes after nExpected successful writes in the batch
                assert.eq(res.writeErrors.length, 1, "number of write errors : " + msg);
                assert.eq(res.writeErrors[0].index, expectedErrorIndex, "error index : " + msg);
                assert.eq(res.writeErrors[0].code, code, "error code : " + msg);
                assert(!res.hasOwnProperty("errorLabels"), msg);
            } catch (e) {
                printjson(cmd);
                printjson(res);
                throw e;
            }
        } finally {
            session.abortTransaction();
        }
    }

    // Run "cmdName" against each collection in "collNames", with combos of "goodOp" and "badOp" in
    // a batch, it should fail with "code".
    function exerciseWriteInTxn({collNames, cmdName, goodOp, badOp, code}) {
        for (let collName of collNames) {
            for (let ordered of[true, false]) {
                let docsField;
                switch (cmdName) {
                    case "insert":
                        docsField = "documents";
                        break;
                    case "update":
                        docsField = "updates";
                        break;
                    case "delete":
                        docsField = "deletes";
                        break;
                }

                // Construct command like {insert: collectionName, documents: [...]}
                let newCmd = () => {
                    var cmd = {};
                    cmd[cmdName] = collName;
                    if (!ordered) {
                        cmd.ordered = false;
                    }

                    return cmd;
                };

                var cmd = newCmd();
                cmd[docsField] = [badOp];
                runInTxn({
                    cmd: cmd,
                    msg: `one bad ${cmdName} on ${collName} collection, ordered ${ordered}`,
                    code: code,
                    nExpected: 0,
                    expectedErrorIndex: 0
                });

                cmd = newCmd();
                cmd[docsField] = [goodOp, badOp];
                let expected = 1;
                if (cmdName == 'delete' && db.getMongo().isMongos()) {
                    // The bad delete write will cause mongos to fail during targetting and not
                    // do any write at all.
                    expected = 0;
                }
                runInTxn({
                    cmd: cmd,
                    msg:
                        `one bad ${cmdName} after a good one on ${collName} collection, ordered ${ordered}`,
                    code: code,
                    nExpected: expected,
                    expectedErrorIndex: 1
                });

                cmd = newCmd();
                cmd[docsField] = [goodOp, goodOp, badOp];
                expected = 2;
                if (cmdName == 'delete' && db.getMongo().isMongos()) {
                    // The bad delete write will cause mongos to fail during targetting and not
                    // do any write at all.
                    expected = 0;
                }
                runInTxn({
                    cmd: cmd,
                    msg:
                        `one bad ${cmdName} after two good ones on ${collName} collection, ordered ${ordered}`,
                    code: code,
                    nExpected: expected,
                    expectedErrorIndex: 2
                });

                cmd = newCmd();
                cmd[docsField] = [goodOp, goodOp, badOp, badOp];
                expected = 2;
                if (cmdName == 'delete' && db.getMongo().isMongos()) {
                    // The bad delete write will cause mongos to fail during targetting and not
                    // do any write at all.
                    expected = 0;
                }
                runInTxn({
                    cmd: cmd,
                    msg:
                        `two bad ${cmdName}s after two good ones on ${collName} collection, ordered ${ordered}`,
                    code: code,
                    nExpected: expected,
                    expectedErrorIndex: 2
                });

                cmd = newCmd();
                cmd[docsField] = [badOp, goodOp];
                runInTxn({
                    cmd: cmd,
                    msg:
                        `good ${cmdName} after a bad one on ${collName} collection, ordered ${ordered}`,
                    code: code,
                    nExpected: 0,
                    expectedErrorIndex: 0
                });
            }
        }
    }

    // Set up a document so we can get a DuplicateKey error trying to insert it again.
    assert.commandWorked(testColl.insert({_id: 5}));
    exerciseWriteInTxn({
        collNames: [testCollName],
        cmdName: "insert",
        goodOp: {},
        badOp: {_id: 5},
        code: ErrorCodes.DuplicateKey
    });

    // Set up a document with a string field so we can update it but fail to increment it.
    assert.commandWorked(testColl.insertOne({_id: 0, x: "string"}));
    exerciseWriteInTxn({
        collNames: [testCollName],
        cmdName: "update",
        goodOp: {q: {_id: 0}, u: {$set: {x: "STRING"}}},
        badOp: {q: {_id: 0}, u: {$inc: {x: 1}}},
        code: ErrorCodes.TypeMismatch
    });

    // Give the good delete operation some documents to delete
    assert.commandWorked(testColl.insertMany([{}, {}, {}, {}]));
    exerciseWriteInTxn({
        collNames: [testCollName],
        cmdName: "delete",
        goodOp: {q: {}, limit: 1},
        badOp: {q: {$foo: 1}, limit: 1},
        code: ErrorCodes.BadValue
    });

    // Capped deletes are prohibited
    runInTxn({
        cmd: {delete: cappedCollName, deletes: [{q: {}, limit: 1}]},
        msg: `delete from ${cappedCollName}`,
        code: ErrorCodes.IllegalOperation,
        nExpected: 0,
        expectedErrorIndex: 0
    });
}());
