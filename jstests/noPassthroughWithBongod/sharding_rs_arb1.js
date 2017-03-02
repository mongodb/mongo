var name = "sharding_rs_arb1";
var replTest = new ReplSetTest({name: name, nodes: 3});
replTest.startSet();
var port = replTest.ports;
replTest.initiate({
    _id: name,
    members: [
        {_id: 0, host: getHostName() + ":" + port[0]},
        {_id: 1, host: getHostName() + ":" + port[1]},
        {_id: 2, host: getHostName() + ":" + port[2], arbiterOnly: true},
    ],
});

replTest.awaitReplication();

var master = replTest.getPrimary();
var db = master.getDB("test");
printjson(rs.status());

var st = new ShardingTest({numShards: 0});
var admin = st.getDB('admin');

var res = admin.runCommand({addshard: replTest.getURL()});
printjson(res);
assert(res.ok, tojson(res));

st.stop();
replTest.stopSet();
