/**
 * Tests NDV command behavior specific to the direct collection scan.
 *
 * @tags: [featureFlagPersistentStats]
 */
import {waitForCurOpByFilter} from "jstests/libs/curop_helpers.js";
import {configureFailPoint, withFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertCommandFailedWithCodeInParallelShell,
    assertCommandWorkedInParallelShell,
} from "jstests/libs/parallel_shell_helpers.js";
import {dropStatsColl} from "jstests/libs/query/persistent_samples_utils.js";

const kFieldStatsCollName = "system.stats.field_stats";

function assertAnalyzeAborted(source, {failPoint, whilePaused, expectedCode}) {
    const db = source.getDB();
    const conn = db.getMongo();
    assert.commandWorked(source.insertOne({a: 1}));
    let awaitAnalyze;
    try {
        withFailPoint(conn, failPoint, (fp) => {
            awaitAnalyze = assertCommandFailedWithCodeInParallelShell(
                conn,
                db,
                {analyze: source.getName(), mode: "ndv", key: "a"},
                expectedCode,
            );
            fp.wait({maxTimeMS: 60 * 1000});
            whilePaused();
        });
    } finally {
        if (awaitAnalyze) {
            awaitAnalyze();
        }
    }
    assert.eq(db[kFieldStatsCollName].countDocuments({}), 0);
}

describe("analyze NDV direct scan", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: true,
                internalQueryEnablePersistentNDVStats: true,
                internalQueryExecYieldIterations: 1,
            },
        });
        this.db = this.conn.getDB(jsTestName());
        this.coll = this.db.source;
        this.fieldStats = this.db[kFieldStatsCollName];
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    beforeEach(function () {
        this.coll.drop();
        dropStatsColl(this.db, kFieldStatsCollName);
        assert.commandWorked(this.db.createCollection(this.coll.getName()));
    });

    it("includes _id in the values fed to the sketch", function () {
        assert.commandWorked(this.coll.insertMany([{_id: 1}, {_id: 2}]));
        assert.commandWorked(
            this.db.runCommand({analyze: this.coll.getName(), mode: "ndv", key: "_id"}),
        );
        assert.eq(this.fieldStats.findOne().ndv.sketches[0].ndv, NumberLong(2));
    });

    it("validates path depth before returning from an empty scan", function () {
        assert.commandFailedWithCode(
            this.db.runCommand({
                analyze: this.coll.getName(),
                mode: "ndv",
                key: Array(201).fill("a").join("."),
            }),
            ErrorCodes.Overflow,
        );
        assert.eq(this.fieldStats.countDocuments({}), 0);
    });

    it("restores caller permissions after persisting stats", function () {
        assert.commandWorked(this.coll.insertOne({a: 1}));
        assert.commandWorked(
            this.db.runCommand({analyze: this.coll.getName(), mode: "ndv", key: "a"}),
        );
        assert.commandFailedWithCode(
            this.db.runCommand({
                aggregate: this.coll.getName(),
                pipeline: [{$merge: {into: kFieldStatsCollName}}],
                cursor: {},
            }),
            31319,
        );
    });

    it("aborts when killed during the scan", function () {
        assertAnalyzeAborted(this.coll, {
            failPoint: "hangCollScanDoWork",
            whilePaused: () => {
                const [op] = waitForCurOpByFilter(this.db, {
                    "command.analyze": this.coll.getName(),
                });
                assert.commandWorked(this.db.killOp(op.opid));
            },
            expectedCode: ErrorCodes.Interrupted,
        });
    });

    it("aborts when the source is recreated after the scan", function () {
        assertAnalyzeAborted(this.coll, {
            failPoint: "hangBeforePersistingNdvStats",
            whilePaused: () => {
                assert.commandWorked(this.db.runCommand({drop: this.coll.getName()}));
                assert.commandWorked(this.db.createCollection(this.coll.getName()));
            },
            expectedCode: ErrorCodes.CollectionUUIDMismatch,
        });
    });

    it("releases the source snapshot during concurrent updates", function () {
        const numDocs = 20;
        assert.commandWorked(
            this.coll.insertMany(Array.from({length: numDocs}, (_, i) => ({_id: i + 1, a: 0}))),
        );

        const comment = "analyze NDV yield";
        const preYield = configureFailPoint(this.conn, "setPreYieldWait", {comment});

        // Read some records before pausing, so a released snapshot would mix old and new values.
        const pauseScan = configureFailPoint(this.conn, "setPreYieldWaitDeferred", {}, {skip: 2});
        const awaitAnalyze = assertCommandWorkedInParallelShell(this.conn, this.db, {
            analyze: this.coll.getName(),
            mode: "ndv",
            key: "a",
            comment,
        });

        try {
            pauseScan.wait({maxTimeMS: 60 * 1000});
            // Replace the single value 0 with numDocs distinct values in the live collection.
            const update = assert.commandWorked(this.coll.updateMany({}, [{$set: {a: "$_id"}}]));
            assert.eq(update.modifiedCount, numDocs);
        } finally {
            pauseScan.off();
            preYield.off();
            awaitAnalyze();
        }

        assert.gt(
            this.fieldStats.findOne().ndv.sketches[0].ndv,
            NumberLong(1),
            "Analyze must release its snapshot and observe updated values",
        );
    });
});
