/**
 * Verifies that successful commits of Sharding DDL operations generate the expected op entry types
 * (following the format and rules defined in the design doc of PM-1939).
 * TODO SERVER-81138 remove multiversion_incompatible and fix comparison with 7.0 binaries
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3, chunkSize: 1});

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
function testMovePrimary() {
    jsTest.log(
        'Testing placement entries added by movePrimary() over a new sharding-enabled DB with no data');

    // Set the initial state
    const dbName = 'movePrimaryTestDB';
    const fromPrimaryShard = st.shard0;
    const fromReplicaSet = st.rs0;
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: fromPrimaryShard.shardName}));

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

testMovePrimary();

st.stop();
