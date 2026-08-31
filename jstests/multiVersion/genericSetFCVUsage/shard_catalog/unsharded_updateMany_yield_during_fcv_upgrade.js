/**
 * Regression test for SERVER-133992: verifies that a partially executed updateMany command is not
 * interrupted by FCV upgrade.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, config: 1});
const db = st.s.getDB("test");
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);

const coll = db.foo;
assert.commandWorked(db.createCollection(coll.getName()));
assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
assert.eq(3, coll.find().itcount());

// Run an updateMany and hang it while yielded after updating some documents.
assert.commandWorked(
    st.rs0.getPrimary().adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
);

const updateFailPoint = configureFailPoint(
    st.rs0.getPrimary(),
    "setYieldAllLocksHang",
    {namespace: coll.getFullName()},
    {skip: 1},
);

const updateThread = new Thread(
    (host, collectionName) => {
        const coll = new Mongo(host).getCollection(collectionName);
        return coll.updateMany({}, {$set: {updated: true}});
    },
    st.s.host,
    coll.getFullName(),
);
updateThread.start();
updateFailPoint.wait();

// While hung, upgrade the FCV
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

// Un-hang the updateMany and verify that it completed successfully and updated all documents.
updateFailPoint.off();
updateThread.join();
assert.commandWorked(updateThread.returnData());
assert.eq(3, coll.countDocuments({}));
assert.eq(3, coll.countDocuments({updated: true}));

st.stop();
