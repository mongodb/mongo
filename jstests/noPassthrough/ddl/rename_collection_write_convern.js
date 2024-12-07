// Verify that the renameCollection command can still be served in presence of a killed replica set
// node.

// The following checks involve talking to a shard node, which in this test is shutdown.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckMetadataConsistency = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

jsTest.log("Testing write concern (2)");

let st = new ShardingTest({
    rs: {nodes: 3},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});
let db = st.getDB("test");

var replTest = st.rs0;

// Kill any node. Don't care if it's a primary or secondary.
replTest.remove(0);

// Call getPrimary() to populate replTest._secondaries.
replTest.getPrimary();
let liveSecondaries = replTest.getSecondaries().filter(function(node) {
    return node.host !== replTest.nodes[0].host;
});
replTest.awaitSecondaryNodes(null, liveSecondaries);
awaitRSClientHosts(st.s, replTest.getPrimary(), {ok: true, ismaster: true}, replTest.name);

assert.commandWorked(db.foo.insert({_id: 4}));
assert.commandWorked(db.foo.renameCollection('bar', true));

st.stop();
