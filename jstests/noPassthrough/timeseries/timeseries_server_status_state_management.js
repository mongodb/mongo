/**
 * Tests that BucketCatalog bucket state management statistics are reported correctly in server
 * status.
 *
 * @tags: [
 *   # State management statistics added as part of scalability improvements project.
 *   requires_fcv_63,
 * ]
 */
const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const db = conn.getDB(dbName);
assert.commandWorked(db.dropDatabase());

const coll = db.getCollection(jsTestName());

const timeFieldName = "tt";
const metaFieldName = "mm";

const resetCollection = () => {
    coll.drop();
    assert.commandWorked(db.createCollection(
        jsTestName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
};

const dropUnrelatedCollection = () => {
    const unrelated = db.getCollection(jsTestName() + "_foo");
    assert.commandWorked(db.createCollection(
        unrelated.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    unrelated.drop();
};

const expected = {
    bucketsManaged: 0,
    currentEra: 0,
    erasWithRemainingBuckets: 0,
    trackedClearOperations: 0
};
const checkServerStatus = function() {
    const actual =
        assert.commandWorked(db.runCommand({serverStatus: 1})).bucketCatalog.stateManagement;
    assert.eq(expected.bucketsManaged, actual.bucketsManaged);
    assert.eq(expected.currentEra, actual.currentEra);
    assert.eq(expected.erasWithRemainingBuckets, actual.erasWithRemainingBuckets);
    assert.eq(expected.trackedClearOperations, actual.trackedClearOperations);
};

resetCollection();
assert.commandWorked(coll.insert({[metaFieldName]: 1, [timeFieldName]: ISODate()}));
expected.bucketsManaged++;
expected.erasWithRemainingBuckets++;
checkServerStatus();

dropUnrelatedCollection();
expected.currentEra++;
expected.trackedClearOperations++;
checkServerStatus();

// Inserting into the existing bucket will update its era, which has no net effect on
// erasWithRemainingBuckets, but the previously tracked clear will get cleaned up.
assert.commandWorked(coll.insert({[metaFieldName]: 1, [timeFieldName]: ISODate()}));
expected.trackedClearOperations--;
checkServerStatus();

assert.commandWorked(coll.insert({[metaFieldName]: 2, [timeFieldName]: ISODate()}));
expected.bucketsManaged++;
checkServerStatus();

// Dropping and recreating the collection will not immediately remove the old bucket states.
resetCollection();
expected.currentEra++;
expected.trackedClearOperations++;
checkServerStatus();

// Inserting more measurements will not remove the old bucket for that meta, as the recreated
// collection has a different UUID. This opens a new one in a new era. The other meta value still
// has an old bucket.
assert.commandWorked(coll.insert({[metaFieldName]: 1, [timeFieldName]: ISODate()}));
expected.bucketsManaged++;
expected.erasWithRemainingBuckets++;
checkServerStatus();

// If we clear an unrelated collection and add a third metadata value, we'll get another bucket in
// a third era.
dropUnrelatedCollection();
assert.commandWorked(coll.insert({[metaFieldName]: 3, [timeFieldName]: ISODate()}));
expected.bucketsManaged++;
expected.currentEra++;
expected.erasWithRemainingBuckets++;
expected.trackedClearOperations++;
checkServerStatus();

MongoRunner.stopMongod(conn);