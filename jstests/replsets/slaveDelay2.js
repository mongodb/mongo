
var name = "slaveDelay2";
var host = getHostName();

var waitForAllMembers = function(master) {
  var ready = false;

  outer:
  while (true) {
    var state = master.getSisterDB("admin").runCommand({replSetGetStatus:1});

    for (var m in state.members) {
      if (state.members[m].state != 2 && state.members[m].state != 1) {
        sleep(10000);
        continue outer;
      }
    }

    printjson(state);
    print("okay, everyone is primary or secondary");
    return;
  }
};


var initialize = function() {
  var replTest = new ReplSetTest( {name: name, nodes: 1} );

  var nodes = replTest.startSet();
  
  replTest.initiate();

  var master = replTest.getMaster().getDB(name);

  waitForAllMembers(master);
  
  return replTest;
};

var populate = function(master) {
  // insert records
  for (var i =0; i<1000; i++) {
    master.foo.insert({_id:1});
  }
  
  master.runCommand({getlasterror:1});
}

doTest = function( signal ) {
  var replTest = initialize();
  var master = replTest.getMaster().getDB(name);
  populate(master);
  var admin = master.getSisterDB("admin");
    
  /**
   * start a slave with a long delay (1 hour) and do some writes while it is
   * initializing. Make sure it syncs all of these writes before going into
   * syncDelay.
   */
  var conn = MongoRunner.runMongod({port : 31008, dbpath : name + "-sd", useHostname: true, replSet: name });
  conn.setSlaveOk();
  
  config = master.getSisterDB("local").system.replset.findOne();
  config.version++;
  config.members.push({_id : 1, host : host+":31008",priority:0, slaveDelay:3600});
  var ok = admin.runCommand({replSetReconfig : config});
  assert.eq(ok.ok,1);

  // do inserts during initial sync
  count = 0;
  while (count < 10) {
    for (var i = 100*count; i<100*(count+1); i++) {
      master.foo.insert({x:i});
    }

    //check if initial sync is done
    var state = master.getSisterDB("admin").runCommand({replSetGetStatus:1});
    printjson(state);
    if (state.members[1].state == 2) {
      break;
    }
    
    count++;
  }
  
  // throw out last 100 inserts, but make sure the others were applied
  if (count == 0) {
    print("NOTHING TO CHECK");
    replTest.stopSet();
    return;
  }

  // wait a bit for the syncs to be applied
  waitForAllMembers(master);    

  for (var i=0; i<(100*count); i++) {
    var obj = conn.getDB(name).foo.findOne({x : i});
    assert(obj);
  }
    
  replTest.stopSet();
}

doTest(15);
