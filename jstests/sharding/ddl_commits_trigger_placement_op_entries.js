/**
 * Verifies that successful commits of Sharding DDL operations generate the expected op entry types
 * (following the format and rules defined in the design doc of PM-1939).
 * TODO SERVER-81138 remove multiversion_incompatible and fix comparison with 7.0 binaries
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_fcv_70,
 *   # TODO (SERVER-100403): Enable this once addShard registers dbs in the shard-local catalog
 *   incompatible_with_authoritative_shards,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3, chunkSize: 1});
const configDB = st.s.getDB('config');

function getExpectedOpEntriesOnNewDb(dbName, primaryShard, isImported = false) {
    // The creation of a database is matched by the generation of two op entries:
    return [
        // - One emitted before the metadata is committed on the sharding catalog
        {
            op: 'n',
            ns: dbName,
            o: {msg: {createDatabasePrepare: dbName}},
            o2: {createDatabasePrepare: dbName, primaryShard: primaryShard, isImported: isImported}
        },
        // - The second one emitted once the metadata is committed on the sharding catalog
        {
            op: 'n',
            ns: dbName,
            o: {msg: {createDatabase: dbName}},
            o2: {createDatabase: dbName, isImported: isImported}
        },
    ];
}

function verifyOpEntriesOnNodes(expectedOpEntryTemplates, nodes) {
    const namespaces = [...new Set(expectedOpEntryTemplates.map(t => t.ns))];
    for (const node of nodes) {
        const foundOpEntries = node.getCollection('local.oplog.rs')
                                   .find({ns: {$in: namespaces}, op: {$in: ['c', 'n']}})
                                   .sort({ts: -1})
                                   .limit(expectedOpEntryTemplates.length)
                                   .toArray()
                                   .reverse();

        assert.eq(expectedOpEntryTemplates.length, foundOpEntries.length);
        for (let i = 0; i < foundOpEntries.length; ++i) {
            assert.eq(expectedOpEntryTemplates[i].op, foundOpEntries[i].op);
            assert.eq(expectedOpEntryTemplates[i].ns, foundOpEntries[i].ns);
            assert.docEq(expectedOpEntryTemplates[i].o, foundOpEntries[i].o);
            assert.docEq(expectedOpEntryTemplates[i].o2, foundOpEntries[i].o2);
        }
    }
}

function testCreateDatabase(dbName = 'createDatabaseTestDB', primaryShardId = st.shard0.shardName) {
    jsTest.log('test createDatabase');

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardId}));

    // Each shard of the cluster should have received the notifications about the database creation
    // (and generated the related op entries).
    const shardPrimaryNodes = Object.values(DiscoverTopology.findConnectedNodes(st.s).shards)
                                  .map(s => new Mongo(s.primary));
    const expectedOpEntries = getExpectedOpEntriesOnNewDb(dbName, primaryShardId);
    verifyOpEntriesOnNodes(expectedOpEntries, shardPrimaryNodes);
}

function testAddShard() {
    jsTest.log('Test addShard');
    const shardPrimaryNodes = Object.values(DiscoverTopology.findConnectedNodes(st.s).shards)
                                  .map(s => new Mongo(s.primary));

    // Create a new replica set and populate it with two DBs
    const newReplicaSet = new ReplSetTest({name: 'addedShard', nodes: 1});
    const newShardName = 'addedShard';
    const preExistingCollName = 'preExistingColl';
    newReplicaSet.startSet({shardsvr: ''});
    newReplicaSet.initiate();
    const dbsOnNewReplicaSet = ['addShardTestDB1', 'addShardTestDB2'];
    for (const dbName of dbsOnNewReplicaSet) {
        const db = newReplicaSet.getPrimary().getDB(dbName);
        assert.commandWorked(db[preExistingCollName].save({value: 1}));
    }

    // Add the new replica set as a shard
    assert.commandWorked(st.s.adminCommand({addShard: newReplicaSet.getURL(), name: newShardName}));

    // Each already existing shard should contain the op entries for each database hosted by
    // newReplicaSet (that have been added to the catalog as part of addShard).
    for (let dbName of dbsOnNewReplicaSet) {
        const expectedOpEntries =
            getExpectedOpEntriesOnNewDb(dbName, newShardName, true /*isImported*/);
        verifyOpEntriesOnNodes(expectedOpEntries, shardPrimaryNodes);
    }

    // Execute the test case teardown
    for (const dbName of dbsOnNewReplicaSet) {
        assert.commandWorked(st.getDB(dbName).dropDatabase());
    }
    let res = assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));
    assert.eq('started', res.state);
    res = assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));
    assert.eq('completed', res.state);
    newReplicaSet.stopSet();
}

function testMovePrimary() {
    jsTest.log(
        'Testing placement entries added by movePrimary() over a new sharding-enabled DB with no data');

    // Set the initial state
    const dbName = 'movePrimaryTestDB';
    const fromPrimaryShard = st.shard0;
    const fromReplicaSet = st.rs0;
    testCreateDatabase(dbName, fromPrimaryShard.shardName);

    // Move the primary shard.
    const toPrimaryShard = st.shard1;
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: toPrimaryShard.shardName}));

    // Verify that the old shard generated the expected event.
    const expectedEntriesForPrimaryMoved = [{
        op: 'n',
        ns: dbName,
        o: {msg: {movePrimary: dbName}},
        o2: {movePrimary: dbName, from: fromPrimaryShard.shardName, to: toPrimaryShard.shardName},
    }];

    verifyOpEntriesOnNodes(expectedEntriesForPrimaryMoved, [fromReplicaSet.getPrimary()]);
}

testCreateDatabase();

testAddShard();

testMovePrimary();

st.stop();
