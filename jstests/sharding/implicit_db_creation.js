/**
 * This tests the basic cases for implicit database creation in a sharded cluster.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const configDB = st.s.getDB('config');

assert.eq(null, configDB.databases.findOne());

const testDB = st.s.getDB('test');

// Test that reads will not result into a new config.databases entry.
assert.eq(null, testDB.user.findOne());
assert.eq(null, configDB.databases.findOne({_id: 'test'}));

assert.commandWorked(testDB.user.insert({x: 1}));

const testDBDoc = configDB.databases.findOne();
assert.eq('test', testDBDoc._id, tojson(testDBDoc));

// Test that inserting to another collection in the same database will not modify the existing
// config.databases entry.
assert.commandWorked(testDB.bar.insert({y: 1}));
assert.eq(testDBDoc, configDB.databases.findOne());

st.s.adminCommand({enableSharding: 'foo'});
assert.neq(null, configDB.databases.findOne({_id: 'foo'}));

st.stop();
