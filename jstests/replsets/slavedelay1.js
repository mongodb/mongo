
var waitForAllMembers = function(master) {
  var ready = false;

  outer:
  while (true) {
    var state = master.getSisterDB("admin").runCommand({replSetGetStatus:1});
    printjson(state);

    for (var m in state.members) {
      if (state.members[m].state != 2 && state.members[m].state != 1) {
        sleep(10000);
        continue outer;
      }
    }
    return;
  }
};


doTest = function( signal ) {

  var name = "slaveDelay";
  var host = getHostName();
  
  var replTest = new ReplSetTest( {name: name, nodes: 3} );

  var nodes = replTest.startSet();

  /* set slaveDelay to 30 seconds */
  var config = replTest.getReplSetConfig();
  config.members[2].priority = 0;
  config.members[2].slaveDelay = 30;
  
  replTest.initiate(config);

  var master = replTest.getMaster().getDB(name);
  var slaveConns = replTest.liveNodes.slaves;
  var slave = [];
  for (var i in slaveConns) {
    var d = slaveConns[i].getDB(name);
    d.getMongo().setSlaveOk();
    slave.push(d);
  }

  waitForAllMembers(master);
  
  // insert a record
  master.foo.insert({x:1});
  master.runCommand({getlasterror:1, w:2});
  
  var doc = master.foo.findOne();
  assert.eq(doc.x, 1);
  
  // make sure slave has it
  var doc = slave[0].foo.findOne();
  assert.eq(doc.x, 1);

  // make sure delayed slave doesn't have it
  assert.eq(slave[1].foo.findOne(), null);

  // wait 35 seconds
  sleep(35000);

  // now delayed slave should have it
  assert.eq(slave[1].foo.findOne().x, 1);

  
  /************* Part 2 *******************/

  // how about non-initial sync?
  
  for (var i=0; i<100; i++) {
    master.foo.insert({_id : i, "foo" : "bar"});
  }
  master.runCommand({getlasterror:1,w:2});

  assert.eq(master.foo.findOne({_id : 99}).foo, "bar");
  assert.eq(slave[0].foo.findOne({_id : 99}).foo, "bar");
  assert.eq(slave[1].foo.findOne({_id : 99}), null);

  sleep(35000); 

  assert.eq(slave[1].foo.findOne({_id : 99}).foo, "bar");
 
  /************* Part 3 *******************/

  // how about if we add a new server?  will it sync correctly?

  var conn = startMongodTest( 31007 , name+"-part3" , 0 , {useHostname : true, replSet : name} );
  
  config = master.getSisterDB("local").system.replset.findOne();
  printjson(config);
  config.version++;
  config.members.push({_id : 3, host : host+":31007",priority:0, slaveDelay:10});

  var admin = master.getSisterDB("admin");
  try {
    var ok = admin.runCommand({replSetReconfig : config});
    assert.eq(ok.ok,1);
  }
  catch(e) {
    print(e);
  }

  master = replTest.getMaster().getDB(name);

  waitForAllMembers(master);

  sleep(15000);

  // it should be all caught up now

  master.foo.insert({_id : 123, "x" : "foo"});
  master.runCommand({getlasterror:1,w:2});

  conn.setSlaveOk();
  assert.eq(conn.getDB(name).foo.findOne({_id:123}), null);
  
  sleep(15000);

  assert.eq(conn.getDB(name).foo.findOne({_id:123}).x, "foo");  
  
  replTest.stopSet();
}

doTest(15);
