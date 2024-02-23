/*
 * Tests that currentOp commands on the replica set endpoint report operations for both router and
 * shard roles and that each operation doc has a "role" field, and that killOp commands on replica
 * set endpoint correctly interrupt the corresponding operations.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagEmbeddedRouter,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";

/*
 * Returns the string representation of the given opId. Removes the 'config:' prefix if exists.
 */
function reformatOpId(opId) {
    if (typeof opId == "string") {
        return opId.replace(/^(config\:)/, "");
    }
    return opId.toString();
}

/*
 * Returns true if the two given opIds are equal, e.g. if opId0 is 1234 or config:1234 and opId2 is
 * 1234 or config:1234.
 */
function areEqualOpIds(opId0, opId1) {
    return reformatOpId(opId0) == reformatOpId(opId1);
}

/*
 * Runs a drop command to drop the collection with the specified database and collection name and
 * returns the response.
 */
function dropCollection(host, dbName, collName) {
    const conn = new Mongo(host);
    return conn.getDB(dbName).runCommand({drop: collName});
}

const st = ShardingTest({
    shards: 1,
    rs: {nodes: 1, setParameter: {featureFlagReplicaSetEndpoint: true}},
    configShard: true,
    embeddedRouter: true,
});

const primary = st.rs0.getPrimary();
const embeddedRouter = new Mongo(primary.routerHost);
const dedicatedMongos = MongoRunner.runMongos({configdb: st.rs0.getURL()});

const dbName = "testDb";
const collName = "testColl";
const primaryTestDB = primary.getDB(dbName);
const primaryTestColl = primaryTestDB.getCollection(collName);

assert.commandWorked(primaryTestColl.insert({x: 1}));
assert.neq(primaryTestColl.findOne({x: 1}), null);

jsTest.log("Start dropCollection through the shard port of the primary and make it hang");
const fp = configureFailPoint(primary, "hangDuringDropCollection");
const dropThread = new Thread(dropCollection, primary.host, dbName, collName);
dropThread.start();
fp.wait();

let clusterDropOpId, shardsvrDropOpId;

{
    jsTest.log("Verify that currentOp on the shard port of the primary returns the in-progress " +
               "router drop command whether or not 'localOps' is set to true");
    // For both cases, the doc should have a "role" field since this is a currentOp command on the
    // replica set endpoint.
    const ops0 =
        primary.getDB("admin")
            .aggregate([{$currentOp: {localOps: true}}, {$match: {"command.drop": collName}}])
            .toArray();
    assert.eq(ops0.length, 1, ops0);
    assert.eq(ops0[0].role, "ClusterRole{router}", ops0);
    assert.eq(ops0[0].host,
              // This is a router op but the originating command came in through the shard port not
              // the router port.
              primary.host,
              ops0);
    clusterDropOpId = ops0[0].opid;

    const ops1 =
        primary.getDB("admin")
            .aggregate([{$currentOp: {localOps: false}}, {$match: {"command.drop": collName}}])
            .toArray();
    assert.eq(ops1.length, 1, ops1);
    assert.eq(ops1[0].role, "ClusterRole{router}", ops1);
    assert.eq(ops1[0].host,
              // This is a router op but the originating command came in through the shard port not
              // the router port.
              primary.host,
              ops1);
    assert(areEqualOpIds(ops1[0].opid, clusterDropOpId), ops1);
}

{
    jsTest.log("Verify that currentOp on the shard port of the primary returns the in-progress " +
               "_shardsvrDropCollection command whether or not 'localOps' is set to true");
    // For both cases, the doc should have a "role" field since this is a currentOp command on the
    // replica set endpoint.
    const ops0 = primary.getDB("admin")
                     .aggregate([
                         {$currentOp: {localOps: true}},
                         {$match: {"command._shardsvrDropCollection": collName}}
                     ])
                     .toArray();
    assert.eq(ops0.length, 1, ops0);
    assert.eq(ops0[0].role, "ClusterRole{shard}", ops0);
    assert.eq(ops0[0].host, primary.host, ops0);
    shardsvrDropOpId = ops0[0].opid

    const ops1 = primary.getDB("admin")
                     .aggregate([
                         {$currentOp: {localOps: false}},
                         {$match: {"command._shardsvrDropCollection": collName}}
                     ])
                     .toArray();
    assert.eq(ops1.length, 1, ops1);
    assert.eq(ops1[0].role, "ClusterRole{shard}", ops1);
    assert.eq(ops1[0].host, primary.host, ops1);
    assert(areEqualOpIds(ops1[0].opid, shardsvrDropOpId), ops1);
}

{
    jsTest.log("Verify that currentOp on the router port of the primary returns the in-progress " +
               "router drop command when 'localOps' is set to true");
    // The doc should not have a "role" field since this is not a currentOp command on the replica
    // set endpoint.
    const ops =
        embeddedRouter.getDB("admin")
            .aggregate([{$currentOp: {localOps: true}}, {$match: {"command.drop": collName}}])
            .toArray();
    assert.eq(ops.length, 1, ops);
    assert(!ops[0].hasOwnProperty("role"), ops);
    assert.eq(ops[0].host,
              // This is a router op but the originating command came in through the shard port not
              // the router port.
              primary.host,
              ops);
    assert(areEqualOpIds(ops[0].opid, clusterDropOpId), ops);
}

{
    jsTest.log("Verify that currentOp on the router port of the primary does not return the " +
               "in-progress router drop command when 'localOps' is set to false");
    const ops =
        embeddedRouter.getDB("admin")
            .aggregate([{$currentOp: {localOps: false}}, {$match: {"command.drop": collName}}])
            .toArray();
    assert.eq(ops.length, 0, ops);
}

{
    jsTest.log("Verify that currentOp on the router port of the primary does not return the " +
               "in-progress _shardsvrDropCollection command when 'localOps' is set to true");
    // The doc should not have a "role" field since this is not a currentOp command on the replica
    // set endpoint.
    const ops = embeddedRouter.getDB("admin")
                    .aggregate([
                        {$currentOp: {localOps: true}},
                        {$match: {"command._shardsvrDropCollection": collName}}
                    ])
                    .toArray();
    assert.eq(ops.length, 0, ops);
}

{
    jsTest.log("Verify that currentOp on the router port of the primary returns the in-progress " +
               "_shardsvrDropCollection command when 'localOps' is set to false");
    // The doc should not have a "role" field since this is not a currentOp command on the replica
    // set endpoint.
    const ops = embeddedRouter.getDB("admin")
                    .aggregate([
                        {$currentOp: {localOps: false}},
                        {$match: {"command._shardsvrDropCollection": collName}}
                    ])
                    .toArray();
    assert.eq(ops.length, 1, ops);
    assert(!ops[0].hasOwnProperty("role"), ops);
    assert.eq(ops[0].host, primary.host, ops);
    assert(areEqualOpIds(ops[0].opid, shardsvrDropOpId), ops);
}

{
    jsTest.log("Verify that currentOp on the dedicated router does not return the in-progress " +
               "router drop command whether or not 'localOps' is set to true since it is not " +
               "the router running the command");
    const ops0 =
        dedicatedMongos.getDB("admin")
            .aggregate([{$currentOp: {localOps: false}}, {$match: {"command.drop": collName}}])
            .toArray();
    assert.eq(ops0.length, 0, ops0);

    const ops1 =
        dedicatedMongos.getDB("admin")
            .aggregate([{$currentOp: {localOps: true}}, {$match: {"command.drop": collName}}])
            .toArray();
    assert.eq(ops1.length, 0, ops1);
}

{
    jsTest.log("Verify that currentOp on the dedicated router does not return the in-progress " +
               "_shardsvrDropCollection command when 'localOps' is set to true");
    // The doc should have a "role" field since this is not a currentOp command on the replica set
    // endpoint.
    const ops = dedicatedMongos.getDB("admin")
                    .aggregate([
                        {$currentOp: {localOps: true}},
                        {$match: {"command._shardsvrDropCollection": collName}}
                    ])
                    .toArray();
    assert.eq(ops.length, 0, ops);
}

{
    jsTest.log("Verify that currentOp on the dedicated router returns the in-progress shard " +
               "_shardsvrDropCollection command when 'localOps' is set to false");
    // The doc should not have a "role" field since this is not a currentOp command on the replica
    // set endpoint.
    const ops = dedicatedMongos.getDB("admin")
                    .aggregate([
                        {$currentOp: {localOps: false}},
                        {$match: {"command._shardsvrDropCollection": collName}}
                    ])
                    .toArray();
    assert.eq(ops.length, 1, ops);
    assert(!ops[0].hasOwnProperty("role"), ops);
    assert.eq(ops[0].host, primary.host, ops);
    assert(areEqualOpIds(ops[0].opid, shardsvrDropOpId), ops);
}

jsTest.log({clusterDropOpId, shardsvrDropOpId});
assert.commandWorked(primaryTestDB.killOp(clusterDropOpId));

{
    jsTest.log("Verify the router drop command got interrupted");
    assert.soon(() => {
        const ops0 =
            primary.getDB("admin")
                .aggregate([{$currentOp: {localOps: true}}, {$match: {"command.drop": collName}}])
                .toArray();
        return ops0.length == 0;
    });

    const ops1 =
        primary.getDB("admin")
            .aggregate([{$currentOp: {localOps: false}}, {$match: {"command.drop": collName}}])
            .toArray();
    assert.eq(ops1.length, 0, ops1);
}

{
    jsTest.log("Verify that _shardsvrDropCollection command did not get interrupted");
    // For both cases, the doc should have a "role" field since this is a currentOp command on the
    // replica set endpoint.
    const ops0 = primary.getDB("admin")
                     .aggregate([
                         {$currentOp: {localOps: true}},
                         {$match: {"command._shardsvrDropCollection": collName}}
                     ])
                     .toArray();
    assert.eq(ops0.length, 1, ops0);
    assert.eq(ops0[0].role, "ClusterRole{shard}", ops0);
    assert.eq(ops0[0].host, primary.host, ops0);
    shardsvrDropOpId = ops0[0].opid

    const ops1 = primary.getDB("admin")
                     .aggregate([
                         {$currentOp: {localOps: false}},
                         {$match: {"command._shardsvrDropCollection": collName}}
                     ])
                     .toArray();
    assert.eq(ops1.length, 1, ops1);
    assert.eq(ops1[0].role, "ClusterRole{shard}", ops1);
    assert.eq(ops1[0].host, primary.host, ops1);
    assert(areEqualOpIds(ops1[0].opid, shardsvrDropOpId), ops1);
}

assert.commandWorked(primaryTestDB.killOp(shardsvrDropOpId));

{
    jsTest.log("Verify that _shardsvrDropCollection command got interrupted");
    assert.soon(() => {
        const ops0 = primary.getDB("admin")
                         .aggregate([
                             {$currentOp: {localOps: true}},
                             {$match: {"command._shardsvrDropCollection": collName}}
                         ])
                         .toArray();
        return ops0.length == 0;
    });

    const ops1 = primary.getDB("admin")
                     .aggregate([
                         {$currentOp: {localOps: false}},
                         {$match: {"command._shardsvrDropCollection": collName}}
                     ])
                     .toArray();
    assert.eq(ops1.length, 0, ops1);
}

assert.commandFailedWithCode(dropThread.returnData(), ErrorCodes.Interrupted);
fp.off();

// The drop operation itself is not interruptible so the collection should still get dropped in the
// background.
assert.soon(() => {
    return primaryTestDB.getCollectionInfos().length == 0;
});

MongoRunner.stopMongos(dedicatedMongos);
st.stop();
