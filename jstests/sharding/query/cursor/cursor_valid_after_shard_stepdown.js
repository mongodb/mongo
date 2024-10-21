import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({
    shards: 1,
    rs: {nodes: 2},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

assert.commandWorked(st.s0.adminCommand({enablesharding: 'TestDB'}));
assert.commandWorked(st.s0.adminCommand({shardcollection: 'TestDB.TestColl', key: {x: 1}}));

var db = st.s0.getDB('TestDB');
var coll = db.TestColl;

// Insert documents for the test
assert.commandWorked(coll.insert({x: 1, value: 'Test value 1'}));
assert.commandWorked(coll.insert({x: 2, value: 'Test value 2'}));

// Establish a cursor on the primary (by not using secondaryOk read)
var findCursor = assert.commandWorked(db.runCommand({find: 'TestColl', batchSize: 1})).cursor;

const primary = st.rs0.getPrimary();

// Stepdown the primary of the shard and ensure that that cursor can still be read
st.rs0.freeze(primary);

var getMoreCursor =
    assert.commandWorked(db.runCommand({getMore: findCursor.id, collection: 'TestColl'})).cursor;
assert.eq(0, getMoreCursor.id);
assert.eq(2, getMoreCursor.nextBatch[0].x);

st.rs0.unfreeze(primary);

st.stop();
