/*
1) Add shard
2) topology time must increase
3) remove shard
4) topology time must increase
*/

function assertTopologyGt(topologyTime1, topologyTime2, msg) {
    let msgError = `[${tojson(topologyTime1)} <= ${tojson(topologyTime2)}] ${msg}`;

    assert.gt(timestampCmp(topologyTime1, topologyTime2), 0, msgError);
}

function cmdAsInternalClient(st, cmd) {
    const command =
        {[cmd]: 1, internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}};
    const connInternal = new Mongo(st.configRS.getPrimary().host);
    const res = assert.commandWorked(connInternal.adminCommand(command));
    connInternal.close();
    return res;
}

function getTopologyTime(st) {
    let res = cmdAsInternalClient(st, "hello");
    return res.$topologyTime;
}

function printConfigShards(st, msg) {
    print(msg, tojson(st.s.getDB("config").shards.find().toArray()));
}

(function() {

'use strict';
load('jstests/sharding/libs/remove_shard_util.js');

var st = new ShardingTest({shards: 1, rs: {nodes: 1}, config: 3});

let initialTopology = getTopologyTime(st);

// AddShard
let rs = new ReplSetTest({name: "rs1", nodes: 1});
rs.startSet({shardsvr: ""});
rs.initiate();
rs.awaitReplication();

assert.commandWorked(st.s.getDB("admin").runCommand({addShard: rs.getURL(), name: "rs1"}));

let topologyTimeAfterAddShard = getTopologyTime(st);

// topology time must increase
assertTopologyGt(topologyTimeAfterAddShard,
                 initialTopology,
                 "Current topologyTime should change after add shard, but it did not");

removeShard(st, "rs1");

printConfigShards(st, "config.shards after remove shard ");

let topologyTimeAfterRemoveShard = getTopologyTime(st);

// topology time should change
assertTopologyGt(topologyTimeAfterRemoveShard,
                 topologyTimeAfterAddShard,
                 "Current topologyTime should change after remove shard, but it did not");

rs.stopSet();
st.stop();
})();
