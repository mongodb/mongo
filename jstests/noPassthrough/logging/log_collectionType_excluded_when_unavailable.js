/**
 * Tests that "collectionType" is omitted from slow query logs in cases where it cannot be
 * accurately determined:
 *   - getMore commands (view context is lost after cursor open)
 *   - Operations routed through mongos (view metadata is not resolved at routing time)
 *
 * TODO SERVER-122872 Add test cases for count and distinct.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkCollectionType} from "jstests/noPassthrough/logging/log_collectionType_utils.js";

const pipeline = [{$project: {_id: 0}}];
const dbName = jsTestName();
const collName = "testColl";

function setupCollection(db) {
    db.setProfilingLevel(0, -1);
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));
}

describe("standalone", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        this.db = this.conn.getDB(dbName);
        setupCollection(this.db);
    });
    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("aggregate on mongod logs collectionType as normal", function () {
        const comment = "standalone_aggregate";
        this.db[collName].aggregate(pipeline, {comment}).toArray();
        checkCollectionType({db: this.db, comment, command: "aggregate", expectedCollType: "normal"});
    });

    it("getMore on mongod omits collectionType", function () {
        const comment = "standalone_getMore";
        const cursor = this.db[collName].aggregate(pipeline, {cursor: {batchSize: 1}, comment});
        cursor.toArray();
        checkCollectionType({db: this.db, comment, command: "getMore", expectedCollType: undefined});
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
        this.db[collName].aggregate(pipeline, {comment}).toArray();
        checkCollectionType({db: this.db, comment, command: "aggregate", expectedCollType: undefined});
    });

    it("getMore on mongos omits collectionType", function () {
        const comment = "sharded_getMore";
        const cursor = this.db[collName].aggregate(pipeline, {cursor: {batchSize: 1}, comment});
        cursor.toArray();
        checkCollectionType({db: this.db, comment, command: "getMore", expectedCollType: undefined});
    });
});
