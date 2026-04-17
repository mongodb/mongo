/**
 * Tests change stream behavior across replica set elections, including:
 * - Change stream cursors survive elections (ReplSetTest sets secondaryOk on member connections,
 *   which the server upconverts to secondaryPreferred, so role changes do not break the cursor)
 * - Change stream readPreference enforcement after elections
 *   (internalChangeStreamRespectsReadPreference: true)
 *
 * @tags: [
 *   uses_change_streams,
 * ]
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {describe, it, before, after, beforeEach, afterEach} from "jstests/libs/mochalite.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("change stream stepdown and readPreference enforcement", function () {
    let replTest;
    let csTest;

    const dbName = jsTestName();
    const collName = "test";

    function stepDown(conn) {
        assert.commandWorked(conn.adminCommand({replSetStepDown: 60, force: true}));
        replTest.awaitSecondaryNodes(null, [conn]);
    }

    function stepUp(conn) {
        assert.commandWorked(conn.adminCommand({replSetFreeze: 0}));
        return replTest.stepUp(conn, {awaitReplicationBeforeStepUp: false});
    }

    before(function () {
        replTest = new ReplSetTest({name: dbName, nodes: [{}, {}]});
        replTest.startSet();
        replTest.initiate();
    });

    afterEach(function () {
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }
        replTest.getPrimary().getDB(dbName)[collName].drop();
    });

    after(function () {
        replTest.stopSet();
    });

    // These tests verify that change stream cursors survive elections. They run regardless of the
    // internalChangeStreamRespectsReadPreference knob state: ReplSetTest sets secondaryOk on
    // member connections, which the server upconverts to secondaryPreferred (see
    // rpc/metadata.cpp extractLegacyReadPreference), so getMore on the stepped-down node still
    // succeeds (the node is now a secondary, which satisfies secondaryPreferred). The knob's
    // enforcement only applies to explicit primary / secondary readPreferences, tested in the
    // block below.
    describe("change stream cursor survives elections", function () {
        let primary, primaryDb;

        beforeEach(function () {
            primary = replTest.getPrimary();
            primaryDb = primary.getDB(dbName);
            csTest = new ChangeStreamTest(primaryDb);
        });

        it("survives stepdown between find and getMore", function () {
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
            });

            assert.commandWorked(primaryDb[collName].insert([{_id: 1}, {_id: 2}, {_id: 3}]));
            replTest.awaitReplication();

            stepDown(primary);

            const res = assert.commandWorked(
                primaryDb.runCommand({getMore: cursor.id, collection: collName, batchSize: 1}),
            );
            assert.eq(res.cursor.nextBatch.length, 1);
            assert.eq(res.cursor.nextBatch[0].fullDocument, {_id: 1});
            assert.eq(res.cursor.nextBatch[0].operationType, "insert");

            stepUp(primary);
        });

        it("survives step-up: cursor opened on secondary works after that node becomes primary", function () {
            const secondary = replTest.getSecondary();
            const secondaryDb = secondary.getDB(dbName);
            // Replace csTest so afterEach cleans up the secondary cursor on failure.
            csTest = new ChangeStreamTest(secondaryDb);

            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
            });

            assert.commandWorked(primaryDb[collName].insert([{_id: 1}, {_id: 2}]));
            replTest.awaitReplication();

            // Step up the secondary — cursor was opened there, now it is the primary.
            stepUp(secondary);

            const res = assert.commandWorked(
                secondaryDb.runCommand({getMore: cursor.id, collection: collName, batchSize: 1}),
            );
            assert.eq(res.cursor.nextBatch.length, 1);
            assert.eq(res.cursor.nextBatch[0].fullDocument, {_id: 1});
            assert.eq(res.cursor.nextBatch[0].operationType, "insert");
        });

        it("survives stepdown between two getMores", function () {
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
            });

            assert.commandWorked(primaryDb[collName].insert([{_id: 1}, {_id: 2}, {_id: 3}]));
            replTest.awaitReplication();

            // First getMore: consume one event before the stepdown.
            let res = assert.commandWorked(
                primaryDb.runCommand({getMore: cursor.id, collection: collName, batchSize: 1}),
            );
            assert.eq(res.cursor.nextBatch.length, 1);

            // Stepdown happens between the two getMores.
            stepDown(primary);

            // Second getMore: cursor should still work on the now-secondary node.
            res = assert.commandWorked(primaryDb.runCommand({getMore: cursor.id, collection: collName, batchSize: 1}));
            assert.eq(res.cursor.nextBatch.length, 1);
            assert.eq(res.cursor.nextBatch[0].fullDocument, {_id: 2});

            stepUp(primary);
        });

        it("sees docs inserted on new primary while waiting on old primary", function () {
            const changeStreamComment = collName + "_comment";
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {comment: changeStreamComment},
            });

            assert.commandWorked(primaryDb[collName].insert([{_id: 1}]));
            replTest.awaitReplication();

            // Consume existing event via raw getMore.
            let res = assert.commandWorked(
                primaryDb.runCommand({getMore: cursor.id, collection: collName, batchSize: 1}),
            );
            assert.eq(res.cursor.nextBatch.length, 1);

            replTest.awaitReplication();

            // Parallel shell: wait for the getMore to appear in $currentOp, then step up the
            // secondary and insert a doc. The original primary's getMore should see this doc.
            async function shellFn(dbName, collName, changeStreamComment) {
                const {ReplSetTest} = await import("jstests/libs/replsettest.js");
                const primary = db.getMongo();
                assert.soon(
                    () =>
                        primary
                            .getDB("admin")
                            .aggregate([
                                {"$currentOp": {}},
                                {
                                    "$match": {
                                        op: "getmore",
                                        "cursor.originatingCommand.comment": changeStreamComment,
                                    },
                                },
                            ])
                            .itcount() == 1,
                );

                const replTest = new ReplSetTest(primary.host);
                const secondary = replTest.getSecondary();
                assert.commandWorked(secondary.adminCommand({replSetFreeze: 0}));
                const newPrimary = replTest.stepUp(secondary, {awaitReplicationBeforeStepUp: false});
                assert.neq(newPrimary, primary, "Primary didn't change.");
                assert.commandWorked(newPrimary.getDB(dbName)[collName].insert({_id: 2}));
            }

            const waitForShell = startParallelShell(
                funWithArgs(shellFn, dbName, collName, changeStreamComment),
                primary.port,
            );

            res = assert.commandWorked(
                primaryDb.runCommand({
                    getMore: cursor.id,
                    collection: collName,
                    batchSize: 1,
                    maxTimeMS: ReplSetTest.kDefaultTimeoutMS,
                }),
            );
            assert.eq(res.cursor.nextBatch.length, 1);
            assert.eq(res.cursor.nextBatch[0].fullDocument, {_id: 2});
            assert.eq(res.cursor.nextBatch[0].operationType, "insert");

            waitForShell();

            // The parallel shell stepped up the secondary; restore the original primary.
            stepUp(primary);
        });
    });

    describe("when internalChangeStreamRespectsReadPreference is enabled", function () {
        function getCursorsForId(db, cursorId) {
            return db
                .getSiblingDB("admin")
                .aggregate([{$currentOp: {idleCursors: true, allUsers: true}}, {$match: {"cursor.cursorId": cursorId}}])
                .toArray();
        }

        function assertCursorPresent(db, cursorId) {
            const cursors = getCursorsForId(db, cursorId);
            assert.eq(cursors.length, 1, "Cursor should have been killed but still exists");
        }

        function assertCursorAbsent(db, cursorId) {
            const cursors = getCursorsForId(db, cursorId);
            assert.eq(cursors.length, 0, "Cursor should have been killed but still exists");
        }

        // Returns true (and skips) if any node in the replica set does not have the query knob
        // enabled. In multiversion, a node on an older binary may not have the enforcement code
        // even if the current primary has the knob. Tests involving role changes must check all
        // nodes because the node receiving getMore after an election may be on the older binary.
        function skipIfQueryKnobIsDisabledOrNotPresent() {
            const isEnabledOnAllNodes = replTest.nodes.every((node) => {
                const res = node
                    .getDB("admin")
                    .adminCommand({getParameter: 1, internalChangeStreamRespectsReadPreference: 1});
                return res.ok && res.internalChangeStreamRespectsReadPreference === true;
            });
            if (!isEnabledOnAllNodes) {
                jsTest.log.info("Skipping: internalChangeStreamRespectsReadPreference is not enabled on all nodes");
                return true;
            }
            return false;
        }

        it("throws InterruptedDueToReplStateChange when primary steps down (readPref: primary)", function () {
            if (skipIfQueryKnobIsDisabledOrNotPresent()) return;
            const primary = replTest.getPrimary();
            const primaryDb = primary.getDB(dbName);

            // Explicitly set readPreference to "primary". Without this, ReplSetTest's
            // secondaryOk flag is upconverted to secondaryPreferred, which wouldn't trigger
            // enforcement.
            let res = primaryDb.runCommand({
                aggregate: collName,
                pipeline: [{$changeStream: {}}],
                cursor: {},
                $readPreference: {mode: "primary"},
            });
            assert.commandWorked(res);
            const cursorId = res.cursor.id;
            assert.neq(cursorId, 0, "Expected a live cursor");

            stepDown(primary);

            assertCursorPresent(primaryDb, cursorId);

            res = primaryDb.runCommand({getMore: cursorId, collection: collName});
            assert.commandFailedWithCode(res, ErrorCodes.InterruptedDueToReplStateChange);

            assertCursorAbsent(primaryDb, cursorId);

            stepUp(primary);
        });

        it("throws InterruptedDueToReplStateChange when secondary becomes primary (readPref: secondary)", function () {
            if (skipIfQueryKnobIsDisabledOrNotPresent()) return;
            const secondary = replTest.getSecondary();
            const secondaryDb = secondary.getDB(dbName);
            replTest.awaitReplication();

            let res = secondaryDb.runCommand({
                aggregate: collName,
                pipeline: [{$changeStream: {}}],
                cursor: {},
                $readPreference: {mode: "secondary"},
            });
            assert.commandWorked(res);
            const cursorId = res.cursor.id;
            assert.neq(cursorId, 0, "Expected a live cursor");

            stepUp(secondary);

            assertCursorPresent(secondaryDb, cursorId);

            res = secondaryDb.runCommand({getMore: cursorId, collection: collName});
            assert.commandFailedWithCode(res, ErrorCodes.InterruptedDueToReplStateChange);

            assertCursorAbsent(secondaryDb, cursorId);
        });

        it("does not throw for primaryPreferred when secondary becomes primary", function () {
            if (skipIfQueryKnobIsDisabledOrNotPresent()) return;
            const secondary = replTest.getSecondary();
            const secondaryDb = secondary.getDB(dbName);
            replTest.awaitReplication();

            csTest = new ChangeStreamTest(secondaryDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "primaryPreferred"}},
            });

            stepUp(secondary);

            // getMore should succeed - primaryPreferred is satisfied by any topology.
            assert.commandWorked(secondaryDb.runCommand({getMore: cursor.id, collection: collName}));
        });

        it("does not throw for secondaryPreferred when secondary becomes primary", function () {
            if (skipIfQueryKnobIsDisabledOrNotPresent()) return;
            const secondary = replTest.getSecondary();
            const secondaryDb = secondary.getDB(dbName);
            replTest.awaitReplication();

            csTest = new ChangeStreamTest(secondaryDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "secondaryPreferred"}},
            });

            stepUp(secondary);

            // getMore should succeed - secondaryPreferred is satisfied by any topology.
            assert.commandWorked(secondaryDb.runCommand({getMore: cursor.id, collection: collName}));
        });

        it("does not throw for nearest when topology changes", function () {
            if (skipIfQueryKnobIsDisabledOrNotPresent()) return;
            const secondary = replTest.getSecondary();
            const secondaryDb = secondary.getDB(dbName);
            replTest.awaitReplication();

            csTest = new ChangeStreamTest(secondaryDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "nearest"}},
            });

            stepUp(secondary);

            // getMore should succeed - nearest is satisfied by any topology.
            assert.commandWorked(secondaryDb.runCommand({getMore: cursor.id, collection: collName}));
        });

        it("resume works after readPreference enforcement error", function () {
            if (skipIfQueryKnobIsDisabledOrNotPresent()) return;
            const primary = replTest.getPrimary();
            const secondary = replTest.getSecondary();
            const primaryDb = primary.getDB(dbName);
            const secondaryDb = secondary.getDB(dbName);

            csTest = new ChangeStreamTest(secondaryDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "secondary"}},
            });

            assert.commandWorked(primaryDb[collName].insert([{_id: 100}]));
            replTest.awaitReplication();

            // Consume the insert event and capture the resume token.
            csTest.getNextChanges(cursor, 1);
            const resumeToken = csTest.getResumeToken(cursor);

            // Step up the secondary -- cursor should fail.
            stepUp(secondary);

            const res = secondaryDb.runCommand({getMore: cursor.id, collection: collName});
            assert.commandFailedWithCode(res, ErrorCodes.InterruptedDueToReplStateChange);

            // After stepUp, the topology is flipped. Get the current state.
            const newPrimary = replTest.getPrimary();
            const newSecondary = replTest.getSecondary();

            assert.commandWorked(newPrimary.getDB(dbName)[collName].insert([{_id: 101}]));
            replTest.awaitReplication();

            // Resume from the token on the current secondary.
            const newSecondaryDb = newSecondary.getDB(dbName);
            csTest = new ChangeStreamTest(newSecondaryDb);
            const resumeCursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
                aggregateOptions: {$readPreference: {mode: "secondary"}},
            });

            csTest.assertNextChangesEqual({
                cursor: resumeCursor,
                expectedChanges: [
                    {
                        operationType: "insert",
                        ns: {db: dbName, coll: collName},
                        fullDocument: {_id: 101},
                        documentKey: {_id: 101},
                    },
                ],
            });
        });

        it("does not affect non-change-stream cursors after election", function () {
            if (skipIfQueryKnobIsDisabledOrNotPresent()) return;
            const primary = replTest.getPrimary();
            const primaryDb = primary.getDB(dbName);

            assert.commandWorked(primaryDb[collName].insert([{_id: 1}, {_id: 2}, {_id: 3}]));
            replTest.awaitReplication();

            // Open a regular find cursor with batchSize: 1 so there are remaining docs to fetch.
            let res = primaryDb.runCommand({
                find: collName,
                batchSize: 1,
                $readPreference: {mode: "primary"},
            });
            assert.commandWorked(res);
            const cursorId = res.cursor.id;
            assert.neq(cursorId, 0, "Expected an open cursor");
            assert.eq(res.cursor.firstBatch.length, 1);

            stepDown(primary);

            // getMore on a non-CS cursor should still succeed -- enforcement is CS-only.
            res = assert.commandWorked(primaryDb.runCommand({getMore: cursorId, collection: collName}));
            assert.gte(res.cursor.nextBatch.length, 1, "Expected remaining docs");

            stepUp(primary);
        });
    });
});
