/**
 * Tests that "collectionType" is omitted from slow query logs in cases where it cannot be
 * accurately determined:
 *   - getMore commands (view context is lost after cursor open)
 *   - Operations routed through mongos (view metadata is not resolved at routing time)
 *
 * Also verifies the basic slow query log fields (collectionType, ns, nreturned, queryShapeHash)
 * for the count and distinct commands.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {
    checkBasicFields,
    checkCollectionType,
} from "jstests/noPassthrough/logging/log_collectionType_utils.js";

const dbName = jsTestName();
const collName = "testColl";
const viewName = "testView";
const tsCollName = "tsColl";

const comments = {
    standaloneCountColl: "standalone_count_coll",
    standaloneDistinctColl: "standalone_distinct_coll",
    standaloneCountView: "standalone_count_view",
    standaloneDistinctView: "standalone_distinct_view",
    standaloneCountTs: "standalone_count_timeseries",
    standaloneDistinctTs: "standalone_distinct_timeseries",
    shardedCount: "sharded_count",
    shardedDistinct: "sharded_distinct",
};

// Each document has a value from a small domain with one duplicate so that:
//   - count() returns 3 (distinguishable from nreturned of 1),
//   - distinct() returns 2 values (distinguishable from both 1 and the number of documents).
function setupCollection(db) {
    db.setProfilingLevel(0, -1);
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 2}]));
}

function setupViewAndTimeseries(db) {
    assert.commandWorked(db.createView(viewName, collName, [{$match: {a: {$gt: 0}}}]));
    assert.commandWorked(
        db.createCollection(tsCollName, {
            timeseries: {timeField: "timestamp", metaField: "metadata"},
        }),
    );
    assert.commandWorked(
        db[tsCollName].insertMany([
            {metadata: {m: 1}, timestamp: ISODate("2021-05-18T00:00:00.000Z"), a: 1},
            {metadata: {m: 1}, timestamp: ISODate("2021-05-18T00:00:01.000Z"), a: 2},
            {metadata: {m: 1}, timestamp: ISODate("2021-05-18T00:00:02.000Z"), a: 2},
        ]),
    );
}

function runCount(db, target, comment) {
    return assert.commandWorked(db.runCommand({count: target, query: {}, comment}));
}

function runDistinct(db, target, comment) {
    return assert.commandWorked(db.runCommand({distinct: target, key: "a", comment}));
}

describe("standalone", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        this.db = this.conn.getDB(dbName);
        setupCollection(this.db);
        setupViewAndTimeseries(this.db);
    });
    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("aggregate on mongod logs collectionType as normal", function () {
        const comment = "standalone_aggregate";
        this.db[collName].aggregate([{$project: {_id: 0}}], {comment}).toArray();
        checkCollectionType({
            db: this.db,
            comment,
            command: "aggregate",
            expectedCollType: "normal",
        });
    });

    it("getMore on mongod omits collectionType", function () {
        const comment = "standalone_getMore";
        const cursor = this.db[collName].aggregate([{$project: {_id: 0}}], {
            cursor: {batchSize: 1},
            comment,
        });
        cursor.toArray();
        checkCollectionType({
            db: this.db,
            comment,
            command: "getMore",
            expectedCollType: undefined,
        });
    });

    it("count on a collection logs collectionType, ns, nreturned and queryShapeHash", function () {
        const comment = comments.standaloneCountColl;
        const res = runCount(this.db, collName, comment);
        assert.eq(3, res.n);
        checkCollectionType({
            db: this.db,
            comment,
            command: "count",
            expectedCollType: "normal",
        });
        checkBasicFields({
            db: this.db,
            comment,
            command: "count",
            expectedNs: `${dbName}.${collName}`,
            expectedNReturned: 1,
        });
    });

    it("distinct on a collection logs collectionType, ns, nreturned and queryShapeHash", function () {
        const comment = comments.standaloneDistinctColl;
        const res = runDistinct(this.db, collName, comment);
        assert.sameMembers([1, 2], res.values);
        checkCollectionType({
            db: this.db,
            comment,
            command: "distinct",
            expectedCollType: "normal",
        });
        checkBasicFields({
            db: this.db,
            comment,
            command: "distinct",
            expectedNs: `${dbName}.${collName}`,
            expectedNReturned: res.values.length,
        });
    });

    it("count on a view logs collectionType as view", function () {
        const comment = comments.standaloneCountView;
        runCount(this.db, viewName, comment);
        checkCollectionType({
            db: this.db,
            comment,
            command: "count",
            expectedCollType: "view",
        });
    });

    it("distinct on a view logs collectionType as view", function () {
        const comment = comments.standaloneDistinctView;
        runDistinct(this.db, viewName, comment);
        checkCollectionType({
            db: this.db,
            comment,
            command: "distinct",
            expectedCollType: "view",
        });
    });

    it("count on a timeseries collection logs the correct collectionType", function () {
        const comment = comments.standaloneCountTs;
        runCount(this.db, tsCollName, comment);
        checkCollectionType({
            db: this.db,
            comment,
            command: "count",
            expectedCollType: areViewlessTimeseriesEnabled(this.db) ? "normal" : "timeseries",
        });
    });

    it("distinct on a timeseries collection logs the correct collectionType", function () {
        const comment = comments.standaloneDistinctTs;
        runDistinct(this.db, tsCollName, comment);
        checkCollectionType({
            db: this.db,
            comment,
            command: "distinct",
            expectedCollType: areViewlessTimeseriesEnabled(this.db) ? "normal" : "timeseries",
        });
    });
});

describe("sharded", function () {
    before(function () {
        this.st = new ShardingTest({shards: 1, mongos: 1});
        this.db = this.st.s.getDB(dbName);
        setupCollection(this.db);
    });
    after(function () {
        this.st.stop();
    });

    it("aggregate on mongos omits collectionType", function () {
        const comment = "sharded_aggregate";
        this.db[collName].aggregate([{$project: {_id: 0}}], {comment}).toArray();
        checkCollectionType({
            db: this.db,
            comment,
            command: "aggregate",
            expectedCollType: undefined,
        });
    });

    it("getMore on mongos omits collectionType", function () {
        const comment = "sharded_getMore";
        const cursor = this.db[collName].aggregate([{$project: {_id: 0}}], {
            cursor: {batchSize: 1},
            comment,
        });
        cursor.toArray();
        checkCollectionType({
            db: this.db,
            comment,
            command: "getMore",
            expectedCollType: undefined,
        });
    });

    it("count on mongos omits collectionType", function () {
        const comment = comments.shardedCount;
        runCount(this.db, collName, comment);
        checkCollectionType({
            db: this.db,
            comment,
            command: "count",
            expectedCollType: undefined,
        });
    });

    it("distinct on mongos omits collectionType", function () {
        const comment = comments.shardedDistinct;
        runDistinct(this.db, collName, comment);
        checkCollectionType({
            db: this.db,
            comment,
            command: "distinct",
            expectedCollType: undefined,
        });
    });
});
