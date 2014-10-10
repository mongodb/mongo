/**
 * Test killing the secondary during initially sync
 *
 * 1. Bring up set
 * 2. Insert some data
 * 4. Make sure synced
 * 5. Freeze #2
 * 6. Bring up #3
 * 7. Kill #2 in the middle of syncing
 * 8. Eventually it should become a secondary
 * 9. Bring #2 back up
 * 10. Insert some stuff
 * 11. Everyone happy eventually
 */

load("jstests/replsets/rslib.js");
var basename = "jstests_initsync1";

print("1. Bring up set");
// SERVER-7455, this test is called from ssl/auth_x509.js
var x509_options1;
var x509_options2;
var replTest = new ReplSetTest({name: basename, 
                                nodes : {node0 : x509_options1, node1 : x509_options2}});

var conns = replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();
var foo = master.getDB("foo");
var admin = master.getDB("admin");

var slave1 = replTest.liveNodes.slaves[0];
var admin_s1 = slave1.getDB("admin");
var local_s1 = slave1.getDB("local");

print("2. Insert some data");
var bulk = foo.bar.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++) {
  bulk.insert({ date: new Date(), x: i, str: "all the talk on the market" });
}
assert.writeOK(bulk.execute());
print("total in foo: "+foo.bar.count());


print("4. Make sure synced");
replTest.awaitReplication();


print("5. Freeze #2");
admin_s1.runCommand({replSetFreeze:999999});


print("6. Bring up #3");
var ports = allocatePorts( 3 );
var basePath = MongoRunner.dataPath + basename;
var hostname = getHostName();

var slave2 = startMongodTest (ports[2],
                              basename,
                              false,
                              Object.merge({replSet : basename, oplogSize : 2}, x509_options2));

var local_s2 = slave2.getDB("local");
var admin_s2 = slave2.getDB("admin");

var config = replTest.getReplSetConfig();
config.version = 2;
config.members.push({_id:2, host:hostname+":"+ports[2]});
try {
  admin.runCommand({replSetReconfig:config});
}
catch(e) {
  print(e);
}
reconnect(slave1);
reconnect(slave2);

wait(function() {
    var config2 = local_s1.system.replset.findOne();
    var config3 = local_s2.system.replset.findOne();

    printjson(config2);
    printjson(config3);

    return config2.version == config.version &&
      (config3 && config3.version == config.version);
  });

wait(function() {
    var status = admin_s2.runCommand({replSetGetStatus:1});
    printjson(status);
    return status.members &&
      (status.members[2].state == 3 || status.members[2].state == 2);
  });


print("7. Kill #2 in the middle of syncing");
replTest.stop(1);


print("8. Eventually it should become a secondary");
print("if initial sync has started, this will cause it to fail and sleep for 5 minutes");
wait(function() {
    var status = admin_s2.runCommand({replSetGetStatus:1});
    occasionally(function() { printjson(status); });
    return status.members[2].state == 2;
    }, 350);


print("9. Bring #2 back up");
replTest.start(1, {}, true);
reconnect(slave1);
wait(function() {
    var status = admin_s1.runCommand({replSetGetStatus:1});
    printjson(status);
    return status.ok === 1 && status.members && status.members.length >= 2 &&
      (status.members[1].state === 2 || status.members[1].state === 1);
  });


print("10. Insert some stuff");
master = replTest.getMaster();
bulk = foo.bar.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++) {
  bulk.insert({ date: new Date(), x: i, str: "all the talk on the market" });
}
assert.writeOK(bulk.execute());

print("11. Everyone happy eventually");
replTest.awaitReplication(300000);

stopMongod(ports[2]);
replTest.stopSet();
