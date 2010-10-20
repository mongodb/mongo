// a replica set's passive nodes should be okay to add as part of a shard config

s = new ShardingTest( "addshard4", 2 , 0 , 1 , {useHostname : true});

r = new ReplSetTest({name : "addshard4", nodes : 3, startPort : 34000});
r.startSet();

var config = r.getReplSetConfig();
config.members[2].priority = 0;

r.initiate(config);

var master = r.getMaster().master;

var members = config.members.map(function(elem) { return elem.host; });
var shardName = "addshard4/"+members.join(",");

print("adding shard "+shardName);

var result = s.adminCommand({"addshard" : shardName});

printjson(result);


