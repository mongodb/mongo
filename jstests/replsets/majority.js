var num = 5;
var host = getHostName();
var name = "tags";
var timeout = 10000;

var replTest = new ReplSetTest( {name: name, nodes: num, startPort:31000} );
var nodes = replTest.startSet();
var port = replTest.ports;
replTest.initiate({_id : name, members :
        [
         {_id:0, host : host+":"+port[0], priority : 2},
         {_id:1, host : host+":"+port[1]},
         {_id:2, host : host+":"+port[2]},
         {_id:3, host : host+":"+port[3], arbiterOnly : true},
         {_id:4, host : host+":"+port[4], arbiterOnly : true},
        ],
                  });

replTest.awaitReplication();
replTest.bridge();

var testInsert = function() {
    master.getDB("foo").bar.insert({x:1});
    var result = master.getDB("foo").runCommand({getLastError:1, w:"majority", wtimeout:timeout});
    printjson(result);
    return result;
};

var master = replTest.getMaster();

print("get back in the groove");
testInsert();
replTest.awaitReplication();

print("makes sure majority works");
assert.eq(testInsert().err, null);

print("setup: 0,1 | 2,3,4");
replTest.partition(0,2);
replTest.partition(0,3);
replTest.partition(0,4);
replTest.partition(1,2);
replTest.partition(1,3);
replTest.partition(1,4);

print("make sure majority doesn't work");
// primary should now be 2
master = replTest.getMaster();
assert.eq(testInsert().err, "timeout");

print("bring set back together");
replTest.unPartition(0,2);
replTest.unPartition(0,3);
replTest.unPartition(1,4);

master = replTest.getMaster();

print("make sure majority works");
assert.eq(testInsert().err, null);

