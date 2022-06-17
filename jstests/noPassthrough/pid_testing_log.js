load("jstests/libs/parallelTester.js");

/**
 * @tags: [requires_replication, requires_sharding]
 *
 * Test that servers set up in testing mode print the
 * pid when they connect as a client to a mongod.
 *
 */

(function() {
"use strict";

const rsMin = 10;
const rsMax = 20;

const baselineParameters = {
    ShardingTaskExecutorPoolMinSize: rsMin,
    ShardingTaskExecutorPoolMaxSize: rsMax,
    ShardingTaskExecutorPoolMinSizeForConfigServers: 4,
    ShardingTaskExecutorPoolMaxSizeForConfigServers: 6,
};

const mongosParameters = Object.assign(
    {logComponentVerbosity: tojson({network: {connectionPool: 5}})}, baselineParameters);

const st = new ShardingTest({
    config: {nodes: 1},
    shards: 1,
    rs0: {nodes: 1},
    mongos: [{setParameter: mongosParameters}],
});
const mongos = st.s0;

const populateTestDb = () => {
    const db = mongos.getDB('test');
    const coll = db.test;
    assert.commandWorked(coll.insert({x: 1}));
};

populateTestDb();

let log = checkLog.getGlobalLog(mongos);
let hits = log.map(line => JSON.parse(line))
               .filter(o => o.msg == "client metadata")
               .filter(o => o.attr.doc.application.pid !== null);

assert(hits.length > 0);

st.stop();
})();
