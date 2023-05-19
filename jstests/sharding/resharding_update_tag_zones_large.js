/**
 * Testing that the reshardCollection command aborts correctly when the transaction for updating
 * the persistent state (e.g. config.collections and config.tags) in the resharding commit phase
 * fails with a TransactionTooLargeForCache error.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function assertEqualObj(lhs, rhs, keysToIgnore) {
    assert.eq(Object.keys(lhs).length, Object.keys(lhs).length, {lhs, rhs});
    for (let key in rhs) {
        if (keysToIgnore && keysToIgnore.has(key)) {
            continue;
        }

        const value = rhs[key];
        if (typeof value === 'object') {
            assertEqualObj(lhs[key], rhs[key], keysToIgnore);
        } else {
            assert.eq(lhs[key], rhs[key], {key, actual: lhs, expected: rhs});
        }
    }
}

const st = new ShardingTest({
    shard: 2,
    // This test uses a fail point to force the commitTransaction command in the resharding commit
    // phase to fail with a TransactionTooLargeForCache error. To make the test setup work reliably,
    // disable the cluster parameter refresher since it periodically runs internal transactions
    // against the the config server.
    mongosOptions: {setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}}
});
const configRSPrimary = st.configRS.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const configDB = st.s.getDB("config");
const collectionsColl = configDB.getCollection("collections");
const chunksColl = configDB.getCollection("chunks");
const tagsColl = configDB.getCollection("tags");

assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: "hashed"}}));

const zoneName = "testZone";
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: zoneName}));

const oldZone = {
    tag: zoneName,
    min: {skey: NumberLong("4470791281878691347")},
    max: {skey: NumberLong("7766103514953448109")}
};
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: oldZone.min, max: oldZone.max, zone: oldZone.tag}));

const collBefore = collectionsColl.findOne({_id: ns});
assert.neq(collBefore, null);
const chunksBefore = chunksColl.find({uuid: collBefore.uuid}).sort({lastmod: -1}).toArray();
assert.gte(chunksBefore.length, 1, chunksBefore);
const tagsBefore = tagsColl.find({ns}).toArray();
assert.gte(tagsBefore.length, 1, tagsBefore);

const reshardingFunc = (mongosHost, ns, zoneName) => {
    const mongos = new Mongo(mongosHost);
    const newZone = {
        tag: zoneName,
        min: {skey: NumberLong("4470791281878691346")},
        max: {skey: NumberLong("7766103514953448108")}
    };
    jsTest.log("Start resharding");
    const reshardingRes = mongos.adminCommand({
        reshardCollection: ns,
        key: {skey: 1},
        unique: false,
        collation: {locale: 'simple'},
        zones: [{zone: newZone.tag, min: newZone.min, max: newZone.max}],
        numInitialChunks: 2,
    });
    jsTest.log("Finished resharding");
    return reshardingRes;
};
let reshardingThread = new Thread(reshardingFunc, st.s.host, ns, zoneName);

const persistFp =
    configureFailPoint(configRSPrimary, "reshardingPauseCoordinatorBeforeDecisionPersisted");
reshardingThread.start();
persistFp.wait();

const commitFp = configureFailPoint(configRSPrimary,
                                    "failCommand",
                                    {
                                        failCommands: ["commitTransaction"],
                                        failInternalCommands: true,
                                        failLocalClients: true,
                                        errorCode: ErrorCodes.TransactionTooLargeForCache,
                                    },
                                    {times: 1});
persistFp.off();
commitFp.wait();
commitFp.off();
const reshardingRes = reshardingThread.returnData();

assert.commandFailedWithCode(reshardingRes, ErrorCodes.TransactionTooLargeForCache);

const collAfter = collectionsColl.findOne({_id: ns});
assert.neq(collAfter, null);
const chunksAfter = chunksColl.find({uuid: collAfter.uuid}).sort({lastmod: -1}).toArray();
const tagsAfter = tagsColl.find({ns}).toArray();

jsTest.log(
    "Verify that the collection metadata remains the same since the resharding operation failed.");

assertEqualObj(collBefore, collAfter);

assert.eq(chunksBefore.length, chunksAfter.length, {chunksBefore, chunksAfter});
for (let i = 0; i < chunksAfter.length; i++) {
    // Ignore "lastmod" when verifying the newest chunk because resharding bumps the minor version
    // of the newest chunk whenever it goes through a state transition.
    assertEqualObj(chunksBefore[i], chunksAfter[i], new Set(i == 0 ? ["lastmod"] : []));
}

assert.eq(tagsBefore.length, tagsAfter.length, {tagsBefore, tagsAfter});
for (let i = 0; i < tagsAfter.length; i++) {
    assertEqualObj(tagsBefore[i], tagsAfter[i]);
}

st.stop();
})();
