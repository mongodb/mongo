
function myprint( x ) {
    print( "chaining output: " + x );
}

var replTest = new ReplSetTest({name: 'testSet', nodes: 3});
var nodes = replTest.startSet();
var hostnames = replTest.nodeList();
replTest.initiate(
    {
        "_id" : "testSet",
        "members" : [
            {"_id" : 0, "host" : hostnames[0], "priority" : 2},
            {"_id" : 1, "host" : hostnames[1]},
            {"_id" : 2, "host" : hostnames[2]}
        ],
        "settings" : {
            "chainingAllowed" : false
        }
    }
);

var master = replTest.getMaster();
replTest.awaitReplication();


var breakNetwork = function() {
    replTest.bridge();
    replTest.partition(0, 2);
    master = replTest.getMaster();
};

var checkNoChaining = function() {
    master.getDB("test").foo.insert({x:1});

    assert.soon(
        function() {
            return nodes[1].getDB("test").foo.findOne() != null;
        }
    );

    var endTime = (new Date()).getTime()+10000;
    while ((new Date()).getTime() < endTime) {
        assert(nodes[2].getDB("test").foo.findOne() == null,
               'Check that 2 does not catch up');
    }
};

var forceSync = function() {
    assert.soon(
        function() {
            var config = nodes[2].getDB("local").system.replset.findOne();
            var targetHost = config.members[1].host;
            printjson(nodes[2].getDB("admin").runCommand({replSetSyncFrom : targetHost}));
            return nodes[2].getDB("test").foo.findOne() != null;
        },
        'Check force sync still works'
    );
};

if (!_isWindows()) {
    print("break the network so that node 2 cannot replicate");
    breakNetwork();

    print("make sure chaining is not happening");
    checkNoChaining();

    print("check that forcing sync target still works");
    forceSync();

    var config = master.getDB("local").system.replset.findOne();
    assert.eq(false, config.settings.chainingAllowed, tojson(config));
}
