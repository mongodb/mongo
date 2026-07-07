/**
 * Tests that findAndModify with sort works on a tracked but unsharded timeseries collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   multiversion_incompatible,
 *   # The test relies on featureFlagTimeseriesUpdatesSupport, which is FCV-gated; suites that pin
 *   # the FCV below latest (e.g. legacy timeseries suites) disable it and reject all single-doc
 *   # timeseries updates.
 *   requires_fcv_90,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const timeField = "t";
const metaField = "m";
const collName = "ts";

describe("findAndModify with sort on tracked unsharded timeseries collection", function () {
    before(function () {
        const tsUpdatesParam = {featureFlagTimeseriesUpdatesSupport: true};
        this.st = new ShardingTest({
            mongos: 1,
            shards: 2,
            rs: {nodes: 1},
            mongosOptions: {setParameter: tsUpdatesParam},
            rsOptions: {setParameter: tsUpdatesParam},
            configOptions: {setParameter: tsUpdatesParam},
        });
        this.testDB = this.st.s.getDB(jsTestName());
        assert.commandWorked(this.testDB.adminCommand({enableSharding: this.testDB.getName()}));
    });

    after(function () {
        this.st.stop();
    });

    function makeTrackedUnshardedTsColl(db, name) {
        // Creating a timeseries collection in a sharded cluster without calling shardCollection
        // yields a collection that is tracked on the config server but unsharded (single-shard).
        const coll = db.getCollection(name);
        coll.drop();
        assert.commandWorked(db.createCollection(name, {timeseries: {timeField, metaField}}));
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, [timeField]: ISODate("2024-01-01T00:00:00Z"), [metaField]: "a", f: 101},
                {_id: 2, [timeField]: ISODate("2024-01-01T00:01:00Z"), [metaField]: "a", f: 102},
                {_id: 3, [timeField]: ISODate("2024-01-01T00:02:00Z"), [metaField]: "b", f: 103},
            ]),
        );
        return coll;
    }

    it("update with sort ascending on f succeeds and returns the minimum matching document", function () {
        const coll = makeTrackedUnshardedTsColl(this.testDB, collName + "_asc");
        const res = assert.commandWorked(
            this.testDB.runCommand({
                findAndModify: coll.getName(),
                query: {f: {$gt: 100}},
                update: {$set: {f: 200}},
                sort: {f: 1},
            }),
        );
        assert.eq(101, res.value.f, "expected pre-image with smallest f value", {res});
    });

    it("update with sort descending on meta field succeeds", function () {
        const coll = makeTrackedUnshardedTsColl(this.testDB, collName + "_meta");
        const res = assert.commandWorked(
            this.testDB.runCommand({
                findAndModify: coll.getName(),
                query: {},
                update: {$set: {f: 999}},
                sort: {[metaField]: -1},
            }),
        );
        assert.neq(null, res.value, "expected to find a document", {res});
    });

    it("remove with sort ascending succeeds and deletes the minimum matching document", function () {
        const coll = makeTrackedUnshardedTsColl(this.testDB, collName + "_remove");
        const res = assert.commandWorked(
            this.testDB.runCommand({
                findAndModify: coll.getName(),
                query: {f: {$gt: 100}},
                remove: true,
                sort: {f: 1},
            }),
        );
        assert.eq(101, res.value.f, "expected pre-image with smallest f value to be removed", {
            res,
        });
        assert.eq(2, coll.countDocuments({}), "expected one document to be removed", {res});
    });

    it("update with sort on a sharded timeseries collection fails with InvalidOptions", function () {
        const coll = this.testDB.getCollection(collName + "_sharded");
        coll.drop();
        assert.commandWorked(
            this.testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
        );
        assert.commandWorked(coll.createIndex({[metaField]: 1}));
        assert.commandWorked(
            this.testDB.adminCommand({
                shardCollection: coll.getFullName(),
                key: {[metaField]: 1},
            }),
        );
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, [timeField]: ISODate("2024-01-01T00:00:00Z"), [metaField]: "a", f: 101},
                {_id: 2, [timeField]: ISODate("2024-01-01T00:01:00Z"), [metaField]: "a", f: 102},
            ]),
        );
        assert.commandFailedWithCode(
            this.testDB.runCommand({
                findAndModify: coll.getName(),
                query: {f: {$gt: 100}},
                update: {$set: {f: 200}},
                sort: {f: 1},
            }),
            ErrorCodes.InvalidOptions,
            "expected findAndModify with sort to fail on a sharded timeseries collection",
        );
    });
});
