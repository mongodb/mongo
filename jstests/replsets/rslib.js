
var count = 0;
var w = 0;

var wait = function(f,msg) {
    w++;
    var n = 0;
    while (!f()) {
        if( n % 4 == 0 )
            print("waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up on ' + msg );
        sleep(1000);
    }
};

/**
 * Use this to do something once every 4 iterations.
 *
 * <pre>
 * for (i=0; i<1000; i++) {
 *   occasionally(function() { print("4 more iterations"); });
 * }
 * </pre>
 */
var occasionally = function(f, n) {
  var interval = n || 4;
  if (count % interval == 0) {
    f();
  }
  count++;
};
  
var reconnect = function(a) {
  wait(function() { 
      try {
        // make this work with either dbs or connections
        if (typeof(a.getDB) == "function") {
          db = a.getDB('foo');
        }
        else {
          db = a;
        }
        db.bar.stats();
        if (jsTest.options().keyFile) { // SERVER-4241: Shell connections don't re-authenticate on reconnect
          return jsTest.authenticate(db.getMongo());
        }
        return true;
      } catch(e) {
        print(e);
        return false;
      }
    });
};


var getLatestOp = function(server) {
    server.getDB("admin").getMongo().setSlaveOk();
    var log = server.getDB("local")['oplog.rs'];
    var cursor = log.find({}).sort({'$natural': -1}).limit(1);
    if (cursor.hasNext()) {
      return cursor.next();
    }
    return null;
};


var waitForAllMembers = function(master, timeout) {
    var failCount = 0;

    assert.soon( function() {
        var state = null
        try {
            state = master.getSisterDB("admin").runCommand({replSetGetStatus:1});
            failCount = 0;
        } catch ( e ) {
            // Connection can get reset on replica set failover causing a socket exception
            print( "Calling replSetGetStatus failed" );
            print( e );
            return false;
        }
        occasionally(function() { printjson(state); }, 10);

        for (var m in state.members) {
            if (state.members[m].state != 1 && // PRIMARY
                state.members[m].state != 2 && // SECONDARY
                state.members[m].state != 7) { // ARBITER
                return false;
            }
        }
        printjson( state );
        return true;
    }, "not all members ready", timeout || 60000);

    print( "All members are now in state PRIMARY, SECONDARY, or ARBITER" );
};

var reconfig = function(rs, config) {
    var admin = rs.getMaster().getDB("admin");
    
    try {
        var ok = admin.runCommand({replSetReconfig : config});
        assert.eq(ok.ok,1);
    }
    catch(e) {
        print(e);
    }

    master = rs.getMaster().getDB("admin");
    waitForAllMembers(master);

    return master;
};
