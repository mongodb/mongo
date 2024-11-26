/**
 * Tests deleteOne works correctly on time-series buckets collections.
 */

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

const mongos = st.s;
const testDB = mongos.getDB(jsTestName());
const collName = "ts";
const bucketsCollName = "system.buckets." + collName;
const timeFieldName = "time";
const metaFieldName = "tag";
const bucketDoc = {
    "_id": ObjectId("64dd4adcac4fd7e3ebbd9af3"),
    "control": {
        "version": 1,
        "min":
            {"_id": ObjectId("64dd4ae9a2c44e75d1151285"), "time": ISODate("2023-08-16T22:17:00Z")},
        "max": {
            "_id": ObjectId("64dd4ae9a2c44e75d1151285"),
            "time": ISODate("2023-08-16T22:17:13.749Z")
        }
    },
    "meta": 1,
    "data": {
        "_id": {"0": ObjectId("64dd4ae9a2c44e75d1151285")},
        "time": {"0": ISODate("2023-08-16T22:17:13.749Z")}
    }
};

function runTest(cmd, validateFn) {
    const coll = testDB.getCollection(collName);
    const bucketsColl = testDB.getCollection(bucketsCollName);
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(),
        {timeseries: {timeField: timeFieldName, metaField: metaFieldName, granularity: "hours"}}));

    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
    assert.commandWorked(
        testDB.adminCommand({shardCollection: coll.getFullName(), key: {[metaFieldName]: 1}}));

    assert.commandWorked(
        testDB.adminCommand({split: testDB[bucketsCollName].getFullName(), middle: {meta: 1}}));
    assert.commandWorked(testDB.adminCommand({
        moveChunk: testDB[bucketsCollName].getFullName(),
        find: {meta: 1},
        to: st.getOther(st.getPrimaryShard(testDB.getName())).shardName,
        _waitForDelete: true
    }));

    // Tests the command works for the buckets collection.
    assert.commandWorked(bucketsColl.insert(bucketDoc));
    assert.commandWorked(testDB.runCommand(cmd));
    validateFn(bucketsColl);
}

function removeValidateFn(coll) {
    assert.eq(coll.count(), 0);
}

runTest({
    delete: bucketsCollName,
    deletes: [{q: {_id: ObjectId("64dd4adcac4fd7e3ebbd9af3")}, limit: 1}]
},
        removeValidateFn);

st.stop();