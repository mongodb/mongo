/*
 * Verify that mongos routes multiUpdates/multiDeletes to _shardsvrCoordinateMultiUpdate when
 * pauseMigrationsDuringMultiUpdates is enabled.
 * @tags: [
 *  requires_fcv_80
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: {rs0: {nodes: 3}}});
const dbName = "test";
const collName = "coll";

assert.commandWorked(st.s0.getDB(dbName).getCollection(collName).insertMany([
    {
        _id: 1,
        member: "abc123",
        points: 0,
    },
    {
        _id: 2,
        member: "abc123",
        points: 100,
    },
]));

assert.commandWorked(
    st.s.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}));

// The setClusterParameter command only sets the cluster parameter on all shards and the config
// server. getClusterParamter will refresh the cluster parameter cache so mongos is able to
// detect that pauseMigrationsDuringMultiUpdates is enabled.
assert.commandWorked(st.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));

const multiUpdateCoodinatorFP =
    configureFailPoint(st.rs0.getPrimary().getDB(dbName), 'hangDuringMultiUpdateCoordinatorRun');

jsTest.log("Running multiUpdate");
const joinMultiUpdateShell =
    startParallelShell(funWithArgs(function(dbName, collName) {
                           const res = db.getSiblingDB(dbName).getCollection(collName).update(
                               {member: "abc123"}, {$set: {points: 50}}, {multi: true});
                           assert.commandWorked(res);
                           assert.eq(res["nModified"], 2, tojson(res));
                       }, dbName, collName), st.s0.port);

multiUpdateCoodinatorFP.wait();
multiUpdateCoodinatorFP.off();
joinMultiUpdateShell();

jsTest.log("Running bulk multiUpdate");
const joinBulkMultiUpdateShell = startParallelShell(
    funWithArgs(function(dbName, collName) {
        const res = db.getSiblingDB(dbName).getCollection(collName).bulkWrite(
            [{updateMany: {filter: {member: "abc123"}, update: {$set: {points: 100}}}}]);
        assert.commandWorked(res);
        assert.eq(res["matchedCount"], 2, tojson(res));
    }, dbName, collName), st.s0.port);

multiUpdateCoodinatorFP.wait();
multiUpdateCoodinatorFP.off();
joinBulkMultiUpdateShell();

jsTest.log("Running multiDelete");
const joinMultiDeleteShell = startParallelShell(
    funWithArgs(function(dbName, collName) {
        const res = db.getSiblingDB(dbName).getCollection(collName).deleteMany({member: "abc123"});
        assert.commandWorked(res);
        assert.eq(res["deletedCount"], 2, tojson(res));
    }, dbName, collName), st.s0.port);

multiUpdateCoodinatorFP.wait();
multiUpdateCoodinatorFP.off();
joinMultiDeleteShell();

st.stop();
