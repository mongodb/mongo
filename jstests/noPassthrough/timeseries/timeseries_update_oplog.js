/**
 * Tests time-series updates are replicated atomically as applyOps oplog entries that group the
 * writes together.
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const timeFieldName = 'time';
const metaFieldName = 'tag';
const dateTime = ISODate("2023-06-29T16:00:00Z");
const testDB = primary.getDB("test");
let collCount = 0;

const initialMeasurement = [
    {_id: 0, [timeFieldName]: dateTime, [metaFieldName]: 0},
    {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 0, a: 1},
    {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: 0, a: 1},
    {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: 1},
];

const runTest = function({cmdBuilderFn, validateFn, retryableWrite = false}) {
    const coll = testDB.getCollection('timeseries_update_oplog' + collCount++);
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.commandWorked(coll.insertMany(initialMeasurement));
    // The above insert may generate an applyOps, so we use startTime to avoid examining it
    // in the validateFn.
    let startTime = testDB.getSession().getOperationTime();

    let cmdObj = cmdBuilderFn(coll);
    if (retryableWrite) {
        const session = primary.startSession({retryWrites: true});
        cmdObj["lsid"] = session.getSessionId();
        cmdObj["txnNumber"] = NumberLong(0);
        assert.commandWorked(session.getDatabase("test").runCommand(cmdObj));
    } else {
        assert.commandWorked(testDB.runCommand(cmdObj));
    }

    validateFn(testDB, coll, retryableWrite, startTime);
};

function partialBucketMultiUpdateBuilderFn(coll) {
    return {update: coll.getName(), updates: [{q: {a: 1}, u: {$inc: {updated: 1}}, multi: true}]};
}
function fullBucketMultiUpdateBuilderFn(coll) {
    return {
        update: coll.getName(),
        updates: [{q: {[metaFieldName]: 0}, u: {$inc: {updated: 1}}, multi: true}]
    };
}
function partialBucketSingletonUpdateBuilderFn(coll) {
    return {
        update: coll.getName(),
        updates: [{q: {[metaFieldName]: 0}, u: {$inc: {updated: 1}}, multi: false}]
    };
}
function fullBucketSingletonUpdateBuilderFn(coll) {
    return {
        update: coll.getName(),
        updates: [{q: {[metaFieldName]: 1}, u: {$inc: {updated: 1}}, multi: false}]
    };
}
function upsertBuilderFn(coll) {
    return {
        update: coll.getName(),
        updates: [{
            q: {[timeFieldName]: dateTime, [metaFieldName]: 2},
            u: {$inc: {updated: 1}},
            multi: false,
            upsert: true
        }]
    };
}

// Full bucket update's oplog entry is an ApplyOps[delete, insert].
function fullBucketValidateFn(testDB, coll, retryableWrite, startTime) {
    const opEntries =
        testDB.getSiblingDB("local")
            .oplog.rs
            .find({
                "o.applyOps.ns": testDB.getName() + '.system.buckets.' + coll.getName(),
                ts: {$gt: startTime}
            })
            .toArray();
    assert.eq(opEntries.length, 1);
    const opEntry = opEntries[0];
    assert.eq(opEntry["o"]["applyOps"].length, 2);
    assert(opEntry["o"]["applyOps"][0]["op"] == "d");
    assert(opEntry["o"]["applyOps"][1]["op"] == "i");
}
// Partial bucket update's oplog entry is an ApplyOps[update, insert].
function partialBucketValidateFn(testDB, coll, retryableWrite, startTime) {
    const opEntries =
        testDB.getSiblingDB("local")
            .oplog.rs
            .find({
                "o.applyOps.ns": testDB.getName() + '.system.buckets.' + coll.getName(),
                ts: {$gt: startTime}
            })
            .toArray();
    assert.eq(opEntries.length, 1);
    const opEntry = opEntries[0];
    assert.eq(opEntry["o"]["applyOps"].length, 2);
    assert(opEntry["o"]["applyOps"][0]["op"] == "u");
    assert(opEntry["o"]["applyOps"][1]["op"] == "i");
}
// When inserting a new measurement, an Upsert's oplog entry is an ApplyOps[insert] if it's a
// retryable write. Otherwise, it generates a regular insert oplog entry.
function upsertValidateFn(testDB, coll, retryableWrite, startTime) {
    const opEntries =
        testDB.getSiblingDB("local")
            .oplog.rs
            .find({
                "o.applyOps.ns": testDB.getName() + '.system.buckets.' + coll.getName(),
                ts: {$gt: startTime}
            })
            .toArray();
    if (retryableWrite) {
        assert.eq(opEntries.length, 1);
        const opEntry = opEntries[0];
        assert.eq(opEntry["o"]["applyOps"].length, 1);
        assert(opEntry["o"]["applyOps"][0]["op"] == "i");
    } else {
        assert.eq(opEntries.length, 0);
    }
}

(function testPartialBucketMultiUpdate() {
    runTest({cmdBuilderFn: partialBucketMultiUpdateBuilderFn, validateFn: partialBucketValidateFn});
})();
(function testFullBucketMultiUpdate() {
    runTest({cmdBuilderFn: fullBucketMultiUpdateBuilderFn, validateFn: fullBucketValidateFn});
})();
(function testPartialBucketSingletonUpdate() {
    runTest(
        {cmdBuilderFn: partialBucketSingletonUpdateBuilderFn, validateFn: partialBucketValidateFn});
})();
(function testPartialBucketSingletonUpdate() {
    runTest({cmdBuilderFn: fullBucketSingletonUpdateBuilderFn, validateFn: fullBucketValidateFn});
})();
(function testPartialBucketRetryableSingletonUpdate() {
    runTest({
        cmdBuilderFn: partialBucketSingletonUpdateBuilderFn,
        validateFn: partialBucketValidateFn,
        retryableWrite: true
    });
})();
(function testPartialBucketRetryableSingletonUpdate() {
    runTest({
        cmdBuilderFn: fullBucketSingletonUpdateBuilderFn,
        validateFn: fullBucketValidateFn,
        retryableWrite: true
    });
})();
(function testUpsert() {
    runTest({cmdBuilderFn: upsertBuilderFn, validateFn: upsertValidateFn});
})();
(function testRetryableUpsert() {
    runTest({cmdBuilderFn: upsertBuilderFn, validateFn: upsertValidateFn, retryableWrite: true});
})();

rst.stopSet();
