/*
 * 1. insert 100000 objects
 * 2. export to two dbpaths
 * 3. add one node w/fastsync
 * 4. check that we never get "errmsg" : "initial sync cloning db: whatever"
 * 5. check writes are replicated
 */

var w = 0;
var wait = function(f) {
    w++;
    var n = 0;
    while (!f()) {
        if( n % 4 == 0 )
            print("toostale.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
}

var reconnect = function(a) {
  wait(function() {
      try {
        a.getDB("foo").bar.stats();
        return true;
      } catch(e) {
        print(e);
        return false;
      }
    });
};

ports = allocatePorts( 4 );

var basename = "jstests_fastsync";
var basePath = "/data/db/" + basename;
var hostname = getHostName();

var pargs = new MongodRunner( ports[ 0 ], basePath + "-p", false, false,
                              ["--replSet", basename, "--oplogSize", 2],
                              {no_bind : true} );
p = pargs.start();

var admin = p.getDB("admin");
var foo = p.getDB("foo");
var local = p.getDB("local");

var config = {_id : basename, members : [{_id : 0, host : hostname+":"+ports[0], priority:2}]};
printjson(config);
var result = admin.runCommand({replSetInitiate : config});
print("result:");
printjson(result);

var count = 0;
while (count < 10 && result.ok != 1) {
  count++;
  sleep(2000);
  result = admin.runCommand({replSetInitiate : config});
}

assert(result.ok, tojson(result));
assert.soon(function() { result = false;
        try {
            result = admin.runCommand({isMaster:1}).ismaster;
        }
        catch(e) {
            print(e);
            return false;
        }
        return result;
    });

print("1");
for (var i=0; i<100000; i++) {
  foo.bar.insert({date : new Date(), x : i, str : "all the talk on the market"});
}
print("total in foo: "+foo.bar.count());


print("2");
admin.runCommand( {fsync:1,lock:1} );
copyDbpath( basePath + "-p", basePath + "-s"+1 );
admin.$cmd.sys.unlock.findOne();

print("3");
var startSlave = function(n) {
    var sargs = new MongodRunner( ports[ n ], basePath + "-s"+n, false, false,
                              ["--replSet", basename, "--fastsync",
                               "--oplogSize", 2], {no_bind : true} );
    var reuseData = true;
    var conn = sargs.start(reuseData);

    config = local.system.replset.findOne();
    config.version++;
    config.members.push({_id:n, host:hostname+":"+ports[n]});

    // When the slave is started, it'll try to load the config and find that it's
    // not in the config and close all connections in preparation for transitioning
    // to "removed" state.  If the reconfig adding it to the set happens to occur at
    // this point, the heartbeat request's connection will be cut off, causing the
    // reconfig to fail..
    assert.soon(function() {
        try {
            result = admin.runCommand({replSetReconfig : config});
        }
        catch (e) {
            print("failed to reconfig: "+e);
            return false;
        }
        return result.ok;
    });
    reconnect(p);

    print("4");
    var status = admin.runCommand({replSetGetStatus : 1});
    var count = 0;
    while (status.members[n].state != 2 && count < 200) {
        print("not a secondary yet");
        if (count % 10 == 0) {
            printjson(status);
        }
        assert(!status.members[n].errmsg || !status.members[n].errmsg.match("^initial sync cloning db"));

        sleep(1000);

        // disconnection could happen here
        try {
            status = admin.runCommand({replSetGetStatus : 1});
        }
        catch (e) {
            print(e);
        }
        count++;
    }

    assert.eq(status.members[n].state, 2);

    assert.soon(function() {
        return admin.runCommand({isMaster : 1}).ismaster;
    });

    admin.foo.insert({x:1});
    assert.soon(function() {
        try {
            var last = local.oplog.rs.find().sort({$natural:-1}).limit(1).next();
            var cur = conn.getDB("local").oplog.rs.find().sort({$natural:-1}).limit(1).next();
            print("last: "+tojson(last)+" cur: "+tojson(cur));
            return cur != null && last != null && cur.ts.t == last.ts.t && cur.ts.i == last.ts.i;
        }
        catch (e) {
            print(e);
        }
        return false;
    });

    return conn;
};

var s1 = startSlave(1);

var me1 = null;

// local.me will not be populated until the secondary reports back to the
// primary that it is syncing
assert.soon(function() {
    me1 = s1.getDB("local").me.findOne();
    if (me1 == null) {
        return false;
    }

    print("me: " +me1._id);
    return me1._id != null;
});

print("5");
s1.getDB("admin").runCommand( {fsync:1,lock:1} );
copyDbpath( basePath + "-s1", basePath + "-s2" );
s1.getDB("admin").$cmd.sys.unlock.findOne();

var s2 = startSlave(2);

var me2 = s2.getDB("local").me.findOne();

print("me: " +me2._id);
assert(me1._id != me2._id);

print("restart member with a different port and make it a new set");
try {
  p.getDB("admin").runCommand({shutdown:1});
}
catch(e) {
  print("good, shutting down: " +e);
}
sleep(10000);

pargs = new MongodRunner( ports[ 3 ], basePath + "-p", false, false,
                          ["--replSet", basename, "--oplogSize", 2],
                          {no_bind : true} );
pargs.start(true);

p = new Mongo("localhost:"+ports[3]);

// initFromConfig will keep closing sockets, so we'll a couple of times
assert.soon(function() {
    try {
        p.getDB("admin").runCommand({replSetReconfig : {
            _id : basename,
            members : [{_id:0, host : hostname+":"+ports[3]}]
        }, force : true});
    }
    catch (e) {
        print(e);
        return false;
    }

    return true;
});

print("start waiting for primary...");
assert.soon(function() {
    try {
      return p.getDB("admin").runCommand({isMaster : 1}).ismaster;
    }
    catch(e) {
      print(e);
    }
    return false;
  }, "waiting for master", 60000);

