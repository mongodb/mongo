/**
 * Tests that time-series inserts respect {ordered: false}.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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
    // We don't guarantee orderedness of inserts based on the metadata value/the order that the
    // documents are inserted in the array; however we know there needs to be at least two inserts
    // that fail because there are two documents with {meta: 0} and we have a non-continuable error.
    //
    // We could have the documents with {meta: 1} be successfully inserted if we have that {meta: 0}
    // documents are committed after the documents with {meta: 1} are inserted.
    assert(resWithCannotContinue.nInserted <= 2);
    assert.eq(resWithCannotContinue.getWriteErrors().length,
              docs.length - resWithCannotContinue.nInserted - 1);

    assert.commandWorked(coll.insert(docs[0]));

    // We now have that we will fail both the inserts with {meta: 0} with continuable errors.
    // This exercises both the "insert" and "update" path when we are attempting to commit a
    // measurement.
    fp = configureFailPoint(failPointConn ? failPointConn : conn,
                            'failUnorderedTimeseriesInsert',
                            {metadata: 0, canContinue: true});

    const res = assert.commandFailed(coll.insert(docs.slice(1), {ordered: false}));

    jsTestLog('Checking insert result: ' + tojson(res));
    // We should successfully insert the two documents with {meta: 1}; these will always be
    // successfully inserted because we now have a continuable error.
    assert.eq(res.nInserted, 2);
    assert.eq(res.getWriteErrors().length, docs.length - res.nInserted - 1);

    // We should only be getting write errors for those documents with {meta: 0}.
    for (let i = 0; i < res.getWriteErrors().length; i++) {
        assert.eq(res.getWriteErrors()[i].getOperation()[metaFieldName], 0);
    }
    fp.off();

    // The documents should go into two new buckets due to the failed insert on the existing
    // bucket.
    assert.commandWorked(coll.insert(docs.slice(1, 3), {ordered: false}));
    assert.eq(bucketsColl.count(),
              2,
              'Expected two buckets but found: ' + tojson(bucketsColl.find().toArray()));
}

runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;
assert.commandWorked(mongos.adminCommand({enableSharding: jsTestName()}));

// Run test on sharded cluster before sharding the collection.
runTest(mongos, st.getPrimaryShard(jsTestName()), false);

// Run test on sharded cluster after sharding the collection.
runTest(mongos, st.getPrimaryShard(jsTestName()), true);
st.stop();
