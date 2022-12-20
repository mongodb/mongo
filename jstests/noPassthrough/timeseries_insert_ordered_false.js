/**
 * Tests that time-series inserts respect {ordered: false}.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');
load('jstests/libs/fail_point_util.js');

const conn = MongoRunner.runMongod();

function runTest(conn, failPointConn, shardColl) {
    const testDB = conn.getDB(jsTestName());

    const coll = testDB.getCollection('t');
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'time';
    const metaFieldName = 'meta';

    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    if (shardColl) {
        assert.commandWorked(conn.adminCommand({
            shardCollection: coll.getFullName(),
            key: {[metaFieldName]: 1},
        }));
    }
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    const docs = [
        {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: 0},
        {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: 0},
        {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: 0},
        {_id: 3, [timeFieldName]: ISODate(), [metaFieldName]: 1},
        {_id: 4, [timeFieldName]: ISODate(), [metaFieldName]: 1},
    ];

    //
    // Test with failPoint which aborts all subsequent write operations of the batch.
    //
    let fp = configureFailPoint(failPointConn ? failPointConn : conn,
                                'failUnorderedTimeseriesInsert',
                                {metadata: 0, canContinue: false});

    const resWithCannotContinue =
        assert.commandFailed(coll.insert(docs.slice(1), {ordered: false}));

    jsTestLog('Checking insert result: ' + tojson(resWithCannotContinue));
    assert.eq(resWithCannotContinue.nInserted, 0);
    assert.eq(resWithCannotContinue.getWriteErrors().length,
              docs.length - resWithCannotContinue.nInserted - 1);
    for (let i = 0; i < resWithCannotContinue.getWriteErrors().length; i++) {
        assert.eq(resWithCannotContinue.getWriteErrors()[i].index, i);
        assert.docEq(docs[i + 1], resWithCannotContinue.getWriteErrors()[i].getOperation());
    }

    //
    // Test with failPoint which can allow subsequent write operations of the batch.
    //
    assert.docEq([], coll.find().sort({_id: 1}).toArray());
    assert.eq(bucketsColl.count(),
              0,
              'Expected zero buckets but found: ' + tojson(bucketsColl.find().toArray()));

    assert.commandWorked(coll.insert(docs[0]));
    fp = configureFailPoint(failPointConn ? failPointConn : conn,
                            'failUnorderedTimeseriesInsert',
                            {metadata: 0, canContinue: true});

    // Insert two documents that would go into the existing bucket and two documents that go into a
    // new bucket.
    const res = assert.commandFailed(coll.insert(docs.slice(1), {ordered: false}));

    jsTestLog('Checking insert result: ' + tojson(res));
    assert.eq(res.nInserted, 2);
    assert.eq(res.getWriteErrors().length, docs.length - res.nInserted - 1);
    for (let i = 0; i < res.getWriteErrors().length; i++) {
        assert.eq(res.getWriteErrors()[i].index, i);
        assert.docEq(docs[i + 1], res.getWriteErrors()[i].getOperation());
    }

    assert.docEq([docs[0], docs[3], docs[4]], coll.find().sort({_id: 1}).toArray());
    assert.eq(bucketsColl.count(),
              2,
              'Expected two buckets but found: ' + tojson(bucketsColl.find().toArray()));

    fp.off();

    // The documents should go into two new buckets due to the failed insert on the existing bucket.
    assert.commandWorked(coll.insert(docs.slice(1, 3), {ordered: false}));
    assert.docEq(docs, coll.find().sort({_id: 1}).toArray());
    // If we allow bucket reopening, we will save out on opening another bucket.
    const expectedBucketCount =
        (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(testDB)) ? 2 : 3;
    assert.eq(bucketsColl.count(),
              expectedBucketCount,
              'Expected three buckets but found: ' + tojson(bucketsColl.find().toArray()));
}

runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;
assert.commandWorked(mongos.adminCommand({enableSharding: jsTestName()}));

// Run test on sharded cluster before sharding the collection.
runTest(mongos, st.getPrimaryShard(jsTestName()), false);

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

// Run test on sharded cluster after sharding the collection.
runTest(mongos, st.getPrimaryShard(jsTestName()), true);
st.stop();
})();
