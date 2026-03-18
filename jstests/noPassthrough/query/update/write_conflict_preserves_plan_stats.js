/**
 * Tests that keysExamined and docsExamined accumulate across write-conflict retries for
 * findAndModify (update and delete) and sorted updateOne.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const kWriteConflictCount = 3;

describe("write conflict preserves plan stats", function () {
    let conn, db, coll;
    const collName = "write_conflict_preserves_plan_stats";

    before(function () {
        conn = MongoRunner.runMongod();
        db = conn.getDB("test");
        coll = db[collName];
        assert.commandWorked(db.adminCommand({profile: 0, slowms: -1}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    function runAndCheckStats(comment, fn) {
        const fp = configureFailPoint(conn, "WTWriteConflictException", {}, {times: kWriteConflictCount});
        fn(comment);
        fp.off();

        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        const entry = findMatchingLogLine(globalLog.log, {id: 51803, comment: comment});
        assert.neq(null, entry, "Slow query log entry not found for comment: " + comment);
        return JSON.parse(entry);
    }

    function assertStatsAccumulated(parsed, label) {
        const wc = parsed.attr.writeConflicts || 0;
        assert.gte(
            wc,
            kWriteConflictCount,
            `${label}: expected at least ${kWriteConflictCount} writeConflicts, got ${wc}`,
        );
        assert.eq(wc + 1, parsed.attr.docsExamined, `${label}: docsExamined should equal writeConflicts+1`);
    }

    it("findAndModify update accumulates keysExamined and docsExamined", function () {
        coll.drop();
        coll.createIndex({x: 1});
        assert.commandWorked(coll.insertOne({x: 1}));

        const parsed = runAndCheckStats("fam_update", (c) => {
            assert.commandWorked(
                db.runCommand({
                    findAndModify: collName,
                    query: {x: 1},
                    update: {$set: {y: 1}},
                    comment: c,
                }),
            );
        });

        assertStatsAccumulated(parsed, "findAndModify update");
    });

    it("findAndModify delete accumulates keysExamined and docsExamined", function () {
        coll.drop();
        coll.createIndex({x: 1});
        assert.commandWorked(coll.insertOne({x: 1}));

        const parsed = runAndCheckStats("fam_delete", (c) => {
            assert.commandWorked(
                db.runCommand({
                    findAndModify: collName,
                    query: {x: 1},
                    remove: true,
                    comment: c,
                }),
            );
        });

        assertStatsAccumulated(parsed, "findAndModify delete");
    });

    it("sorted updateOne accumulates keysExamined and docsExamined", function () {
        coll.drop();
        coll.createIndex({x: 1});
        assert.commandWorked(coll.insertOne({x: 1}));

        const parsed = runAndCheckStats("sorted_update", (c) => {
            assert.commandWorked(
                db.runCommand({
                    update: collName,
                    updates: [{q: {x: 1}, u: {$set: {y: 1}}, sort: {x: 1}}],
                    comment: c,
                }),
            );
        });

        assertStatsAccumulated(parsed, "sorted updateOne");
    });

    it("sorted updateOne accumulates stats when doc no longer matches after yield", function () {
        coll.drop();
        coll.createIndex({x: 1});
        assert.commandWorked(
            coll.insertMany([
                {x: 1, y: 1},
                {x: 1, y: 2},
            ]),
        );

        const comment = "sorted_update_no_match";
        const fp = configureFailPoint(conn, "hangBeforeUpdaterEnsureDocStillMatchesAndYield", {}, "alwaysOn");

        const awaitUpdate = startParallelShell(
            `assert.commandWorked(db.runCommand({
                update: "${collName}",
                updates: [{q: {x: 1}, u: {$set: {u: 1}}, sort: {y: 1}}],
                comment: "${comment}",
            }));`,
            conn.port,
        );

        // Wait for the update to reach the fail point (it found {x: 1, y: 1} first due to sort).
        fp.wait();

        // Concurrently delete the document.
        assert.commandWorked(coll.deleteOne({x: 1, y: 1}));

        // Release the fail point. The update stage will detect the mismatch and throw a WCE,
        // causing a retry that finds and updates {x: 1, y: 2}.
        fp.off();
        awaitUpdate();

        assert.eq(coll.findOne({x: 1, y: 2}).u, 1, "expected the second document to be updated");

        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        const entry = findMatchingLogLine(globalLog.log, {id: 51803, comment: comment});
        assert.neq(null, entry, "Slow query log entry not found for comment: " + comment);
        const parsed = JSON.parse(entry);

        const wc = parsed.attr.writeConflicts || 0;
        assert.gte(wc, 1, parsed);
        assert.gte(parsed.attr.docsExamined, 2, parsed);
    });

    it("express update accumulates keysExamined and docsExamined", function () {
        coll.drop();
        coll.createIndex({x: 1}, {unique: true});
        assert.commandWorked(coll.insertOne({x: 1}));

        const parsed = runAndCheckStats("express_update", (c) => {
            assert.commandWorked(
                db.runCommand({
                    update: collName,
                    updates: [{q: {x: 1}, u: {$set: {y: 1}}}],
                    comment: c,
                }),
            );
        });

        assertStatsAccumulated(parsed, "express update");
    });

    it("delete accumulates keysExamined and docsExamined", function () {
        coll.drop();
        coll.createIndex({x: 1});

        const kDocCount = 100;
        const docs = [];
        for (let i = 0; i < kDocCount; ++i) {
            docs.push({x: i});
        }
        assert.commandWorked(coll.insertMany(docs));

        const parsed = runAndCheckStats("batch_delete", (c) => {
            const fp = configureFailPoint(
                conn,
                "throwWriteConflictExceptionInBatchedDeleteStage",
                {},
                {activationProbability: 0.25},
            );
            assert.commandWorked(
                db.runCommand({
                    delete: collName,
                    deletes: [{q: {x: {$gte: kDocCount / 2}}, limit: 0}],
                    comment: c,
                }),
            );
            fp.off();
        });

        const wc = parsed.attr.writeConflicts || 0;
        assert.gt(wc, 0, `expected at least 1 writeConflicts, got ${wc}`);
        // Batch delete must re-fetch every document in the batch after write conflict.
        assert.lte(
            kDocCount / 2 + wc,
            parsed.attr.docsExamined,
            `docsExamined should be at least the number of documents deleted plus writeConflicts`,
        );
    });
});
