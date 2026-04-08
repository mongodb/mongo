/**
 * Tests that validate timeseries collections that have extended range timestamps.
 *
 * @tags: [
 * requires_persistence,
 * requires_fcv_83
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("Validation of timeseries collection with extended-range timestamps", function () {
    const collNameBase = jsTestName();
    this.ord = 0;
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(jsTestName());
    });

    beforeEach(function () {
        this.ord = this.ord + 1; // Use a different collection name for each test
        this.collName = `${collNameBase}.${this.ord}`;
        this.db[this.collName].drop(); // drop any potentially stale collection
        assert.commandWorked(
            this.db.createCollection(this.collName, {
                timeseries: {timeField: "t", metaField: "m", granularity: "seconds"},
            }),
        );
        this.coll = this.db.getCollection(this.collName);
    });

    it("Successfully validates normal workload with extended range timestamps", () => {
        // Simulate a workload with a few timestamps pre-epoch, post-epoch, and post epochalypse
        const n = 1000;
        for (let ts of [-50000, 50000, "2040-01-01T00:00:00Z"]) {
            let timestamp = new Date(ts);
            const millisecondsPerMinute = 1000 * 60;
            assert.commandWorked(
                this.coll.insertMany(
                    [...Array(n).keys()].map(() => {
                        timestamp.setSeconds(timestamp.getSeconds() + 1);
                        const measurement = Math.sin((2 * Math.PI * timestamp.getTime()) / millisecondsPerMinute);
                        return {
                            t: timestamp,
                            data: {
                                temp: measurement,
                            },
                        };
                    }),
                ),
            );
        }
        const res = assert.commandWorked(this.coll.validate());
        assert(res.valid, res);
        assert.eq(res.warnings.length, 0, res);
    });

    it("Passes validation with extended range timestamps before epoch", function () {
        assert.commandWorked(this.coll.insert({t: new Date(-100), data: {a: "b"}})); // Pre-epoch using integer
        assert.commandWorked(this.coll.insert({t: new Date("1950-01-01T00:00:00Z"), data: {x: "y"}})); // Pre-epoch using UTC timestamp notation
        const res = assert.commandWorked(this.coll.validate());
        assert(res.valid, res);
        assert.eq(res.warnings.length, 0, res);
    });

    it("Passes validation with extended range timestamps after epochalypse", function () {
        assert.commandWorked(this.coll.insert({t: new Date("2040-01-01T00:00:01Z"), data: {a: "b"}})); // Post Jan 19, 2038
        assert.commandWorked(this.coll.insert({t: new Date("2040-01-01T00:00:02Z"), data: {x: "y"}})); // Post Jan 19, 2038
        const res = assert.commandWorked(this.coll.validate());
        assert(res.valid, res);
        assert.eq(res.warnings.length, 0, res);
    });

    it("Passes validation with extended range timestamps before and after epoch", function () {
        assert.commandWorked(this.coll.insert({t: new Date("1969-12-31T23:59:58Z"), data: {a: "b"}})); // Pre-epoch
        assert.commandWorked(this.coll.insert({t: new Date("1969-12-31T23:59:59Z"), data: {a: "b"}})); // Pre-epoch
        assert.commandWorked(this.coll.insert({t: new Date("1970-01-01T00:00:00Z"), data: {x: "y"}})); // Post-epoch
        assert.commandWorked(this.coll.insert({t: new Date("1970-01-01T00:00:01Z"), data: {x: "y"}})); // Post-epoch
        const res = assert.commandWorked(this.coll.validate());
        assert(res.valid, res);
        assert.eq(res.warnings.length, 0, res);
    });

    it("Passes validation with extended range timestamps before and after epochalypse", function () {
        assert.commandWorked(this.coll.insert({t: new Date("2038-01-19T03:14:05Z"), data: {a: "b"}})); // Pre-epochalypse
        assert.commandWorked(this.coll.insert({t: new Date("2038-01-19T03:14:06Z"), data: {a: "b"}})); // Pre-epochalypse
        assert.commandWorked(this.coll.insert({t: new Date("2038-01-19T03:14:07Z"), data: {x: "y"}})); // Post-epochalypse
        assert.commandWorked(this.coll.insert({t: new Date("2038-01-19T03:14:08Z"), data: {x: "y"}})); // Post-epochalypse
        const res = assert.commandWorked(this.coll.validate());
        assert(res.valid, res);
        assert.eq(res.warnings.length, 0, res);
    });

    it("Passes validation with metadata cardinality > 1 and split across the epoch", function () {
        const docs = [
            {
                "_id": 35,
                "t": ISODate("1969-12-31T23:59:59.989Z"),
                "m": {
                    "m1": true,
                    "m2": ISODate("1970-01-01T00:00:00.024Z"),
                },
            },
            {
                "_id": 171,
                "t": ISODate("1970-01-01T00:00:00.001Z"),
                "m": {
                    "m1": NumberInt(-265145177),
                    "m2": NumberInt(-1752677305),
                },
            },
        ];
        assert.commandWorked(this.coll.insert(docs));
        const res = assert.commandWorked(this.coll.validate());
        assert(res.valid, res);
        assert.eq(res.warnings.length, 0, res);
    });

    afterEach(function () {
        this.coll.drop();
    });

    after(function () {
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });
});

describe("Validation against snapshot using Replset", function () {
    const collNameBase = jsTestName();
    this.ord = 0;

    before(function () {
        this.rs = new ReplSetTest({nodes: 1});
        this.rs.startSet();
        this.rs.initiate();
    });

    beforeEach(function () {
        this.ord += 1;
        this.collName = `${collNameBase}.${this.ord}`;
        this.db = this.rs.getPrimary().getDB(jsTestName());
        this.db[this.collName].drop(); // drop any potentially stale collection

        assert.commandWorked(this.db.createCollection(this.collName, {timeseries: {timeField: "time"}}));
        this.coll = this.db.getCollection(this.collName);

        assert.commandWorked(this.coll.insert({time: ISODate("3026-02-10T18:31:35.788Z")}));
    });

    it("Passes background validation with extended range timestamps", function () {
        // Take a checkpoint.
        assert.commandWorked(this.db.adminCommand({fsync: 1}));
        assert(this.coll.drop());

        // TODO: SERVER-119598
        // Background validation operates on a snapshot, and there is a known issue where
        // the in-memory flag for extended range timestamps is not being set upon validation
        // on the snapshot after the collection is dropped and the collection shared state is
        // cleaned up.  Background validation will bypass extended range checks to prevent
        // failures until this issue can be addressed.

        // TODO: SERVER-120042 It should not be required to re-create the collection for validation on a snapshot
        assert.commandWorked(this.db.createCollection(this.collName, {timeseries: {timeField: "time"}}));

        const res = this.db[this.collName].validate({background: true});
        assert(res.valid, res);
        assert.eq(res.nrecords, 1, {reason: "Expected at least 1 record from the time the snapshot was made", res});

        assert.eq(res.warnings.length, 0, res);
        assert.eq(res.errors.length, 0, res);
    });

    it("Passes foreground validation with specified atClusterTime", function () {
        // Take a checkpoint.
        const clusterTime = assert.commandWorked(this.db.adminCommand({fsync: 1})).$clusterTime.clusterTime;
        assert(this.coll.drop());

        // TODO: SERVER-120042 It should not be required to re-create the collection for validation on a snapshot
        assert.commandWorked(this.db.createCollection(this.collName, {timeseries: {timeField: "time"}}));

        const res = this.db[this.collName].validate({atClusterTime: clusterTime});
        assert(res.valid, res);
        assert.eq(res.nrecords, 1, {reason: "Expected at least 1 record from the time the snapshot was made", res});

        assert.eq(res.warnings.length, 0, res);
        assert.eq(res.errors.length, 0, res);
    });

    after(function () {
        this.rs.stopSet({skipValidation: true});
    });
});
