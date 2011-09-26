

var replTest = new ReplSetTest( {name: 'unicomplex', nodes: 2} );
var conns = replTest.startSet();
replTest.initiate();

// Make sure we have a master
var master = replTest.getMaster();

for (i=0;i<10000; i++) { master.getDB("bar").foo.insert({x:1,y:i,abc:123,str:"foo bar baz"}); }
for (i=0;i<1000; i++) { master.getDB("bar").foo.update({y:i},{$push :{foo : "barrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr"}}); }

replTest.awaitReplication();

assert.soon(function() { return conns[1].getDB("admin").isMaster().secondary; });

join = startParallelShell( "db.getSisterDB('bar').runCommand({compact : 'foo'});", replTest.ports[1] );

print("check secondary goes to recovering");
assert.soon(function() { return !conns[1].getDB("admin").isMaster().secondary; });

print("joining");
join();

print("check secondary becomes a secondary again");
var secondarySoon = function() {
    var x = 0;
    assert.soon(function() {
        var im = conns[1].getDB("admin").isMaster();
        if (x++ % 5 == 0) printjson(im);
        return im.secondary;
    });
};

secondarySoon();

print("make sure compact works on a secondary (SERVER-3923)");
master.getDB("foo").bar.drop();
replTest.awaitReplication();
var result = conns[1].getDB("foo").runCommand({compact : "bar"});
assert.eq(result.ok, 0, tojson(result));

secondarySoon();

print("use replSetMaintenance command to go in/out of maintence mode");

print("primary cannot go into maintence mode");
result = master.getDB("admin").runCommand({replSetMaintenance : 1});
assert.eq(result.ok, 0, tojson(result));

print("secondary can");
result = conns[1].getDB("admin").runCommand({replSetMaintenance : 1});
assert.eq(result.ok, 1, tojson(result));

print("make sure secondary goes into recovering");
var x = 0;
assert.soon(function() {
    var im = conns[1].getDB("admin").isMaster();
    if (x++ % 5 == 0) printjson(im);
    return !im.secondary && !im.ismaster;
});

result = conns[1].getDB("admin").runCommand({replSetMaintenance : 0});
assert.eq(result.ok, 1, tojson(result));

secondarySoon();