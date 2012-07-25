load("jstests/replsets/rslib.js");

doTest = function( signal ) {

  var name = "slaveDelay";
  var host = getHostName();

  var replTest = new ReplSetTest( {name: name, nodes: 3} );

  var nodes = replTest.startSet();

  /* set slaveDelay to 30 seconds */
  var config = replTest.getReplSetConfig();
  config.members[2].priority = 0;
  config.members[2].slaveDelay = 10;

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

  for (var i=0; i<8; i++) {
      assert.eq(slave[1].foo.findOne(), null);
      sleep(1000);
  }

  // now delayed slave should have it
  assert.soon(function() {
          var z = slave[1].foo.findOne();
          return z && z.x == 1;
      });


  /************* Part 2 *******************/

  // how about non-initial sync?

  for (var i=0; i<100; i++) {
    master.foo.insert({_id : i, "foo" : "bar"});
  }
  master.runCommand({getlasterror:1,w:2});

  assert.eq(master.foo.findOne({_id : 99}).foo, "bar");
  assert.eq(slave[0].foo.findOne({_id : 99}).foo, "bar");
  assert.eq(slave[1].foo.findOne({_id : 99}), null);

  for (var i=0; i<8; i++) {
      assert.eq(slave[1].foo.findOne({_id:99}), null);
      sleep(1000);
  }

  assert.soon(function() {
          var z = slave[1].foo.findOne({_id : 99});
          return z && z.foo == "bar";
      });

  /************* Part 3 *******************/

  // how about if we add a new server?  will it sync correctly?

  conn = replTest.add();

  config = master.getSisterDB("local").system.replset.findOne();
  printjson(config);
  config.version++;
  config.members.push({_id : 3, host : host+":"+replTest.ports[replTest.ports.length-1],priority:0, slaveDelay:10});

  master = reconfig(replTest, config);
  master = master.getSisterDB(name);

  // it should be all caught up now

  master.foo.insert({_id : 123, "x" : "foo"});
  master.runCommand({getlasterror:1,w:2});

  conn.setSlaveOk();

  for (var i=0; i<8; i++) {
      assert.eq(conn.getDB(name).foo.findOne({_id:123}), null);
      sleep(1000);
  }

  assert.soon(function() {
          var z = conn.getDB(name).foo.findOne({_id:123});
          return z != null && z.x == "foo"
      });

  /************* Part 4 ******************/

  print("reconfigure slavedelay");

  config.version++;
  config.members[3].slaveDelay = 15;

  reconfig(replTest, config);
  master = replTest.getMaster().getDB(name);
  assert.soon(function() {
          return conn.getDB("local").system.replset.findOne().version == config.version;
      });

  assert.soon(function() {
      var result = conn.getDB("admin").isMaster();
      printjson(result);
      return result.secondary;
  });

  print("testing insert");
  master.foo.insert({_id : 124, "x" : "foo"});
  assert(master.foo.findOne({_id:124}) != null);

  for (var i=0; i<13; i++) {
      assert.eq(conn.getDB(name).foo.findOne({_id:124}), null);
      sleep(1000);
  }

  replTest.awaitReplication();

  replTest.stopSet();
}

doTest(15);
