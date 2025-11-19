/**
 * Tests basic CRUD operations on the local DB of an non-initiated replica set.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

for (const opts of [{}, {shardsvr: ""}, {configsvr: ""}]) {
    jsTest.log.info(`Testing basic DDL and CRUD on local DB with options: ${tojsononeline(opts)}`);

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet(opts);
    const conn = rst.nodes[0];

    // Since we didn't initiate the replica set, we are on uninitialized FCV
    assert.commandFailedWithCode(
        conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}),
        ErrorCodes.UnknownFeatureCompatibilityVersion,
    );

    const localDB = conn.getDB("local");
    const coll = localDB.getCollection("clustermanager");

    assert.commandWorked(localDB.createCollection(coll.getName()));
    assert.eq("collection", coll.getMetadata().type);
    assert(coll.drop());

    assert.commandWorked(coll.insertOne({x: 1}));
    assert.commandWorked(coll.insertMany([{x: 2}, {x: 3}]));
    assert.commandWorked(coll.updateOne({x: 2}, {$inc: {a: 1}}));
    assert.commandWorked(coll.updateMany({}, {$inc: {a: 1}}));
    assert.commandWorked(coll.deleteMany({x: {$gt: 2}}));

    assert.sameMembers(
        [
            {x: 1, a: 1},
            {x: 2, a: 2},
        ],
        coll.find({}, {_id: 0}).toArray(),
    );
    assert.eq(2, coll.count({}));
    assert.sameMembers([{x: 2, a: 2}], coll.aggregate([{$sort: {x: -1}}, {$limit: 1}, {$project: {_id: 0}}]).toArray());

    rst.stopSet();
}
