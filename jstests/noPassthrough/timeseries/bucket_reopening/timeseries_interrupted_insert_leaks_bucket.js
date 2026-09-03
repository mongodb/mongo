/**
 * Reproduces a scenario where the timeseries bucket catalog would leak write
 * batches if staging a batch failed (e.g. due to InterruptedException if the
 * operation is killed). All previously staged batches would be left staged,
 * resulting in a bucket that could never be closed. In addition, if the zombie
 * batches originally created the bucket, a subsequent write could insert into
 * that empty bucket, materializing a bucket where control.min.t reflects a
 * measurement that was never actually written.
 *
 * @tags: [requires_timeseries]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

describe("time-series bucket leaked by an interrupted insert", function () {
    const dbName = jsTestName();
    const collName = "coll";
    const timeField = "t";
    const metaField = "m";

    // The measurement whose bucket leaks. Minute-aligned so its rounded bucket min equals the
    // measurement time itself.
    const leakedTime = ISODate("2000-01-01T10:00:00Z");
    // Anchor for the archived bucket that the interrupted insert tries to reopen.
    const archiveAnchor = new Date(leakedTime.getTime() + 3 * 60 * 60 * 1000);
    // Time-backward from 'archiveAnchor' by more than bucketMaxSpanSeconds (1h), so inserting it
    // archives the anchor's bucket.
    const archiveTrigger = new Date(archiveAnchor.getTime() - 2 * 60 * 60 * 1000);
    // Within the archived bucket's [min, min + bucketMaxSpanSeconds) window, and time-forward by
    // more than bucketMaxSpanSeconds from 'leakedTime', so staging it after 'leakedTime' rolls the
    // fresh bucket over and reaches archive-based reopening.
    const reopenTarget = new Date(archiveAnchor.getTime() + 30 * 60 * 1000);
    // Within bucketMaxSpanSeconds of 'leakedTime', so a leaked bucket would accept it, but with a
    // distinct rounded bucket min of its own.
    const probeTime = new Date(leakedTime.getTime() + 5 * 60 * 1000);

    // Times for the unordered case below. Minute-aligned, on a different day so they cannot share a
    // bucket with anything above, and one per meta value so that the insert stages one write batch
    // per measurement.
    const unorderedTimes = [ISODate("2000-01-02T10:00:00Z"), ISODate("2000-01-02T11:00:00Z")];
    const unorderedMetas = [1, 2];
    const unorderedProbeTimes = unorderedTimes.map((t) => new Date(t.getTime() + 5 * 60 * 1000));

    let conn;
    let db;
    let coll;

    const rawBuckets = (filter) => {
        const rawColl = getTimeseriesCollForRawOps(db, coll);
        return rawColl.find(filter).rawData().toArray();
    };

    // Kills the insert parked on 'failPoint', then releases it and waits for it to exit. The parked
    // insert only observes the kill once it resumes.
    const killParkedInsert = (failPoint, joinInsert) => {
        try {
            const ops = db
                .getSiblingDB("admin")
                .aggregate([
                    {$currentOp: {}},
                    {$match: {"command.insert": collName, "command.$db": dbName}},
                ])
                .toArray();
            assert.eq(1, ops.length, "expected exactly one parked insert", {ops});
            assert.commandWorked(db.adminCommand({killOp: 1, op: ops[0].opid}));
        } finally {
            failPoint.off();
            joinInsert();
        }
    };

    before(function () {
        conn = MongoRunner.runMongod({});
        db = conn.getDB(dbName);
        coll = db[collName];
        assert.commandWorked(db.createCollection(collName, {timeseries: {timeField, metaField}}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("does not stage later measurements into the leaked bucket", function () {
        // Set up an archived bucket: opening a bucket at 'archiveAnchor' and then inserting
        // time-backward archives it.
        assert.commandWorked(coll.insert({_id: 100, [metaField]: 0, [timeField]: archiveAnchor}));
        assert.commandWorked(coll.insert({_id: 101, [metaField]: 0, [timeField]: archiveTrigger}));

        // Insert a two-measurement command. Within one meta context, measurements stage in time
        // order: 'leakedTime' first allocates a fresh bucket (no committed measurements, no
        // document on disk) and registers a write batch in it; 'reopenTarget' then rolls that
        // bucket over and reaches archive-based reopening of the 'archiveAnchor' bucket, where the
        // insert parks on an interruptible failpoint.
        const hangBeforeFetch = configureFailPoint(
            conn,
            "hangTimeseriesReopenArchivedBucketBeforeFetch",
        );
        const interruptedInsert = startParallelShell(
            funWithArgs(
                function (dbName, collName, metaField, timeField, tsLeaked, tsReopen) {
                    const res = db.getSiblingDB(dbName).runCommand({
                        insert: collName,
                        documents: [
                            {_id: 0, [metaField]: 0, [timeField]: tsLeaked},
                            {_id: 1, [metaField]: 0, [timeField]: tsReopen},
                        ],
                    });
                    jsTest.log.info("Interrupted insert result", {res});
                    assert.commandFailedWithCode(res, ErrorCodes.Interrupted);
                },
                dbName,
                collName,
                metaField,
                timeField,
                leakedTime,
                reopenTarget,
            ),
            conn.port,
        );
        hangBeforeFetch.wait();

        // Kill the parked insert. The interruptible failpoint throws, and the code must abort the
        // batch registered for the 'leakedTime' bucket.
        killParkedInsert(hangBeforeFetch, interruptedInsert);

        // The interrupted command must not have written anything.
        assert.eq(0, coll.find({_id: {$in: [0, 1]}}).itcount());
        jsTest.log.info("Bucket catalog after the interrupted insert", {
            bucketCatalog: db.serverStatus().bucketCatalog,
        });

        // If the 'leakedTime' bucket leaked, this measurement is staged into it and committed as a
        // full-document insert built from the leaked in-memory state.
        assert.commandWorked(coll.insert({_id: 2, [metaField]: 0, [timeField]: probeTime}));

        const leaked = rawBuckets({"control.min.t": leakedTime});
        assert.eq(
            0,
            leaked.length,
            "a measurement was staged into a bucket leaked by the interrupted insert",
            {leaked},
        );
        const probeBuckets = rawBuckets({"control.min.t": probeTime});
        assert.eq(1, probeBuckets.length, "expected the probe measurement to open its own bucket", {
            allBuckets: rawBuckets({}),
        });

        const validation = assert.commandWorked(coll.validate());
        assert(validation.valid, "collection failed validation", {validation});
    });

    it("does not stage later measurements into buckets left behind by an interrupted unordered insert", function () {
        // An unordered insert with one measurement per meta value stages one write batch per
        // measurement, then commits them one at a time. Park the command after staging but before
        // committing any batch, and kill it: the batch being committed is aborted by
        // commitTimeseriesBucketForBatch, but the batches after it were never committed and must
        // still be aborted by the caller, or their buckets stay open in the catalog with zero
        // committed measurements and no document on disk.
        const hangBeforeCommit = configureFailPoint(conn, "hangTimeseriesInsertBeforeCommit");
        const interruptedInsert = startParallelShell(
            funWithArgs(
                function (dbName, collName, metaField, timeField, metas, times) {
                    const res = db.getSiblingDB(dbName).runCommand({
                        insert: collName,
                        ordered: false,
                        documents: metas.map((meta, i) => ({
                            _id: 200 + i,
                            [metaField]: meta,
                            [timeField]: times[i],
                        })),
                    });
                    jsTest.log.info("Interrupted unordered insert result", {res});
                    assert.commandFailedWithCode(res, ErrorCodes.Interrupted);
                },
                dbName,
                collName,
                metaField,
                timeField,
                unorderedMetas,
                unorderedTimes,
            ),
            conn.port,
        );
        hangBeforeCommit.wait();

        // Once the insert resumes, the commit of the first batch throws and the remaining batches
        // unwind.
        killParkedInsert(hangBeforeCommit, interruptedInsert);

        // The interrupted command must not have written anything.
        assert.eq(0, coll.find({_id: {$in: [200, 201]}}).itcount());
        jsTest.log.info("Bucket catalog after the interrupted unordered insert", {
            bucketCatalog: db.serverStatus().bucketCatalog,
        });

        // If any of the staged buckets leaked, its meta's probe measurement is staged into it and
        // committed as a full-document insert built from the leaked in-memory state.
        for (let i = 0; i < unorderedMetas.length; i++) {
            assert.commandWorked(
                coll.insert({
                    _id: 300 + i,
                    [metaField]: unorderedMetas[i],
                    [timeField]: unorderedProbeTimes[i],
                }),
            );
        }

        for (let i = 0; i < unorderedMetas.length; i++) {
            const leaked = rawBuckets({
                meta: unorderedMetas[i],
                "control.min.t": unorderedTimes[i],
            });
            assert.eq(
                0,
                leaked.length,
                "a measurement was staged into a bucket leaked by the interrupted unordered insert",
                {leaked},
            );
            const probeBuckets = rawBuckets({
                meta: unorderedMetas[i],
                "control.min.t": unorderedProbeTimes[i],
            });
            assert.eq(
                1,
                probeBuckets.length,
                "expected the probe measurement to open its own bucket",
                {allBuckets: rawBuckets({})},
            );
        }

        const validation = assert.commandWorked(coll.validate());
        assert(validation.valid, "collection failed validation", {validation});
    });
});
