// @tags: [requires_replication, requires_sharding]

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let name = "sharding_rs_arb1";
let replTest = new ReplSetTest({name: name, nodes: 3, nodeOptions: {shardsvr: ""}});
replTest.startSet();
let port = replTest.ports;
replTest.initiate({
    _id: name,
    members: [
        {_id: 0, host: getHostName() + ":" + port[0]},
        {_id: 1, host: getHostName() + ":" + port[1]},
        {_id: 2, host: getHostName() + ":" + port[2], arbiterOnly: true},
    ],
});

replTest.awaitReplication();

let primary = replTest.getPrimary();
var db = primary.getDB("test");
printjson(rs.status());

let st = new ShardingTest({numShards: 0});
let admin = st.getDB("admin");

// Setting CWWC for addShard to work, as implicitDefaultWC is set to w:1.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
let res = admin.runCommand({addshard: replTest.getURL()});
printjson(res);
assert(res.ok, tojson(res));

st.stop();
replTest.stopSet();
