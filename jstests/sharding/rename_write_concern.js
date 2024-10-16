// The following checks involve talking to a shard node, which in this test is shutdown.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckMetadataConsistency = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

jsTest.log("Testing write concern (2)");

let st = new ShardingTest({rs: {nodes: 3}});
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
