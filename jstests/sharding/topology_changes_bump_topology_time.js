/*
1) Add shard
2) topology time must increase
3) remove shard
4) topology time must increase
*/

function cmdAsInternalClient(st, cmd) {
    const command =
        {[cmd]: 1, internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}};
    const connInternal = new Mongo(st.configRS.getPrimary().host);
    const res = assert.commandWorked(connInternal.adminCommand(command));
    connInternal.close();
    return res;
}

function getTopologyTimeAsJson(st) {
    let res = cmdAsInternalClient(st, "hello");
    return tojson(res.$topologyTime);
}

function printConfigShards(st, msg) {
    print(msg, tojson(st.s.getDB("config").shards.find().toArray()));
}

(function() {

'use strict';

var st = new ShardingTest({shards: 1, rs: {nodes: 1}, config: 3});

let initialTopology = getTopologyTimeAsJson(st);

// AddShard
let rs = new ReplSetTest({name: "rs1", nodes: 1});
rs.startSet({shardsvr: ""});
rs.initiate();
rs.awaitReplication();

assert.commandWorked(st.s.getDB("admin").runCommand({addShard: rs.getURL(), name: "rs1"}));

let topologyTimeAfterAddShard = getTopologyTimeAsJson(st);

// topology time must increase
assert.gt(topologyTimeAfterAddShard,
          initialTopology,
          "Current topologyTime should change after add shard, but it did not");

assert.commandWorked(st.s.adminCommand({removeShard: "rs1"}));
printConfigShards(st, "config.shards after first remove shard ");

assert.commandWorked(st.s.adminCommand({removeShard: "rs1"}));
printConfigShards(st, "config.shards after second remove shard ");

let topologyTimeAfterRemoveShard = getTopologyTimeAsJson(st);

// topology time should change
assert.gt(topologyTimeAfterRemoveShard,
          topologyTimeAfterAddShard,
          "Current topologyTime should change after remove shard, but it did not");

rs.stopSet();
st.stop();
})();
